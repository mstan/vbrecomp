"""V810 → C emitter.

Consumes `DecodedInstruction` directly and produces compileable C for
each function discovered by `analysis.discover_functions`. No IR
in-between — for P3 bootstrap, a thin direct emitter is simpler to
debug and aligns with how `snesrecomp/v2` ships its codegen.

Output layout per game (file names parametrised by `module_name`):

    generated/<name>_full.c       — every recompiled function body
    generated/<name>_dispatch.c   — vb_dispatch / vb_dispatch_call
    generated/<name>.h            — vb_fn_<va> prototypes

Per CLAUDE.md Rule 4 the generated files are build artifacts — they
are NEVER hand-edited. If the C is wrong, fix this emitter.

Per CLAUDE.md Rule 0 the emitter NEVER emits a stub. An opcode it
cannot translate causes a vb_stub_abort at runtime via the
`vb_stub_abort("...");` fallthrough, and a `# TODO` comment in the
generated source so it's visible during code review.

Calling convention (mirrors psxrecomp's; CLAUDE.md §0/§14):

    void vb_fn_<va>(CPUState* cpu);

The function reads/writes `cpu->gpr[N]`, `cpu->psw_*`, `cpu->sysreg`,
and the bus accessors `cpu->read*/write*`. It is responsible for
updating `cpu->pc` at each instruction boundary so debug-server
state-fetches always see a consistent PC.

Control flow:

  * Bcond / JR within the function   → C goto into a leader label
  * Bcond / JR / JAL leaving fn      → vb_dispatch / vb_dispatch_call
  * JMP r31                          → return                       (*)
  * JMP rX, X != 31                  → vb_dispatch(cpu, cpu->gpr[X])
  * HALT                             → cpu->halted = 1; return
  * RETI                              → vb_reti(cpu); return
  * TRAP                              → vb_trap(cpu, imm5); return

(*) JMP r31 compiles to `return` because the caller checks whether
    r31 was modified between entry and exit; an unmodified r31 means
    the recompiled callee returned cleanly, a modified r31 means the
    callee transferred control and we route the new target through
    vb_dispatch. See `runtime/src/dispatch_glue.c` (P3 wiring).
"""
from __future__ import annotations

from dataclasses import dataclass
from typing import Dict, Iterable, List, Optional, Set, Tuple

from .analysis import (
    BasicBlock,
    ControlFlowGraph,
    FunctionRange,
    RomImage,
    build_cfg,
    cfg_walk_with_table_resolution,
    discover_functions,
    trace_reset_trampoline,
)
from .cycles import BCOND_TAKEN_EXTRA, instr_base_cycles
from .decoder import DecodedInstruction
from .isa import Format


# Bcond condition expression as a C boolean over the exploded PSW.
# Maps the 4-bit cond field to its true-condition C predicate.
_BCOND_C_EXPR: Dict[int, str] = {
    0x0: "cpu->psw_ov",                                # BV
    0x1: "cpu->psw_cy",                                # BL / BC
    0x2: "cpu->psw_z",                                 # BE / BZ
    0x3: "(cpu->psw_cy | cpu->psw_z)",                 # BNH
    0x4: "cpu->psw_s",                                 # BN
    0x5: "1",                                          # BR (always)
    0x6: "(cpu->psw_s ^ cpu->psw_ov)",                 # BLT
    0x7: "((cpu->psw_s ^ cpu->psw_ov) | cpu->psw_z)",  # BLE
    0x8: "(!cpu->psw_ov)",                             # BNV
    0x9: "(!cpu->psw_cy)",                             # BNL / BNC
    0xA: "(!cpu->psw_z)",                              # BNE / BNZ
    0xB: "(!(cpu->psw_cy | cpu->psw_z))",              # BH
    0xC: "(!cpu->psw_s)",                              # BP
    0xD: "0",                                          # NOP (never)
    0xE: "(!(cpu->psw_s ^ cpu->psw_ov))",              # BGE
    0xF: "(!((cpu->psw_s ^ cpu->psw_ov) | cpu->psw_z))",  # BGT
}


def _fn_symbol(pc: int) -> str:
    """Canonical C identifier for a recompiled function at `pc`."""
    return f"vb_fn_{pc & 0xFFFFFFFF:08X}"


def _bb_label(pc: int) -> str:
    """Canonical local label name for a basic-block leader at `pc`."""
    return f"bb_{pc & 0xFFFFFFFF:08X}"


# ---------- Per-instruction emit ----------

def _emit_format_i(ins: DecodedInstruction) -> str:
    """Format I — register-register (2 bytes). reg2 is dest + lhs;
    reg1 is rhs (or just the source for MOV/NOT). All ALU ops update
    PSW Z/S; arithmetic ops also set OV/CY."""
    r1, r2 = ins.reg1, ins.reg2
    op = ins.opcode6
    # r0 writes are no-ops in hardware; suppress to avoid clobbering
    # the constant. Reads of r0 still resolve through gpr[0] which the
    # runtime initialises to 0.
    write_dest = (r2 != 0)
    if op == 0x00:  # MOV reg1, reg2 — copy, no PSW
        if not write_dest:
            return ""
        return f"cpu->gpr[{r2}] = cpu->gpr[{r1}];"
    if op == 0x01:  # ADD
        body = (f"uint32_t _a=cpu->gpr[{r2}], _b=cpu->gpr[{r1}]; "
                f"uint32_t _r=_a+_b; "
                + (f"cpu->gpr[{r2}]=_r; " if write_dest else "")
                + "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                  "cpu->psw_cy=(_r<_a); "
                  "cpu->psw_ov=(((~(_a^_b))&(_a^_r))>>31)&1;")
        return "{ " + body + " }"
    if op == 0x02:  # SUB
        body = (f"uint32_t _a=cpu->gpr[{r2}], _b=cpu->gpr[{r1}]; "
                f"uint32_t _r=_a-_b; "
                + (f"cpu->gpr[{r2}]=_r; " if write_dest else "")
                + "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                  "cpu->psw_cy=(_a<_b); "
                  "cpu->psw_ov=(((_a^_b)&(_a^_r))>>31)&1;")
        return "{ " + body + " }"
    if op == 0x03:  # CMP (read-only — no dest write)
        body = (f"uint32_t _a=cpu->gpr[{r2}], _b=cpu->gpr[{r1}]; "
                "uint32_t _r=_a-_b; "
                "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                "cpu->psw_cy=(_a<_b); "
                "cpu->psw_ov=(((_a^_b)&(_a^_r))>>31)&1;")
        return "{ " + body + " }"
    if op in (0x04, 0x05, 0x07):  # SHL / SHR / SAR
        c_op = {"0x04": "<<", "0x05": ">>", "0x07": ">>"}[f"0x{op:02X}"]
        is_sar = (op == 0x07)
        body = (f"uint32_t _a=cpu->gpr[{r2}]; "
                f"uint32_t _sh=cpu->gpr[{r1}]&0x1F; "
                "uint32_t _r; "
                + (f"_r=(uint32_t)((int32_t)_a{c_op}_sh);"
                   if is_sar else f"_r=_a{c_op}_sh;")
                + "cpu->psw_cy=_sh? ((_a>>("
                + ("_sh-1)" if op == 0x04 else "_sh-1)" if op != 0x04 else "")
                + " )&1) : 0; "
                + (f"cpu->gpr[{r2}]=_r; " if write_dest else "")
                + "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                  "cpu->psw_ov=0;")
        # The CY bit definition for shifts is "the last bit shifted out"
        # (V810 manual). For SHL it's bit (32-sh); for SHR/SAR it's
        # bit (sh-1). We compute the common-case "bit (sh-1) shifted
        # out the right" for SHR/SAR; for SHL we override below.
        if op == 0x04:
            body = (f"uint32_t _a=cpu->gpr[{r2}]; "
                    f"uint32_t _sh=cpu->gpr[{r1}]&0x1F; "
                    "uint32_t _r=_a<<_sh; "
                    "cpu->psw_cy = _sh ? ((_a>>(32-_sh))&1) : 0; "
                    + (f"cpu->gpr[{r2}]=_r; " if write_dest else "")
                    + "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                      "cpu->psw_ov=0;")
        elif op in (0x05, 0x07):
            cast = "(int32_t)" if is_sar else ""
            body = (f"uint32_t _a=cpu->gpr[{r2}]; "
                    f"uint32_t _sh=cpu->gpr[{r1}]&0x1F; "
                    f"uint32_t _r=(uint32_t)({cast}_a>>_sh); "
                    "cpu->psw_cy = _sh ? ((_a>>(_sh-1))&1) : 0; "
                    + (f"cpu->gpr[{r2}]=_r; " if write_dest else "")
                    + "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                      "cpu->psw_ov=0;")
        return "{ " + body + " }"
    if op == 0x06:  # JMP rX — handled by control-flow layer
        # Should not reach here; the block emitter handles JMP as a
        # terminator. If we do reach, emit a fatal abort.
        return f"vb_stub_abort_simple(\"unexpected JMP r{r1} in straight-line emit\", 0x{ins.pc:08X}u);"
    if op == 0x08:  # MUL — signed 32x32 -> 64; r30:reg2 = result
        # Oracle order (v810_oploop.inc:816-819): write r30 (high word)
        # FIRST, then the destination (low word) — so when dest==r30 the
        # destination write WINS. Z/S come from the destination's FINAL
        # value (the low word, or 0 when dest is r0), NOT the full 64-bit
        # product. OV = the product doesn't fit a signed 32.
        body = (f"int64_t _s=(int64_t)(int32_t)cpu->gpr[{r2}]*"
                f"(int64_t)(int32_t)cpu->gpr[{r1}]; "
                "uint32_t _lo=(uint32_t)_s; "
                "cpu->gpr[30]=(uint32_t)((uint64_t)_s>>32); "
                + (f"cpu->gpr[{r2}]=_lo; " if write_dest else "")
                + f"cpu->psw_z=(cpu->gpr[{r2}]==0); "
                  f"cpu->psw_s=(cpu->gpr[{r2}]>>31)&1; "
                  "cpu->psw_ov=(_s!=(int64_t)(int32_t)_lo);")
        return "{ " + body + " }"
    if op == 0x0A:  # MULU — unsigned 32x32 -> 64
        # Same r30-first / dest-last order and low-word Z/S as MUL
        # (v810_oploop.inc:827-831). OV = high word non-zero.
        body = (f"uint64_t _s=(uint64_t)cpu->gpr[{r2}]*(uint64_t)cpu->gpr[{r1}]; "
                "uint32_t _lo=(uint32_t)_s; "
                "cpu->gpr[30]=(uint32_t)(_s>>32); "
                + (f"cpu->gpr[{r2}]=_lo; " if write_dest else "")
                + f"cpu->psw_z=(cpu->gpr[{r2}]==0); "
                  f"cpu->psw_s=(cpu->gpr[{r2}]>>31)&1; "
                  "cpu->psw_ov=((_s>>32)!=0);")
        return "{ " + body + " }"
    if op == 0x09:  # DIV — signed; quotient in reg2, remainder in r30
        # Oracle (v810_oploop.inc:858-883): ÷0 raises the V810 zero-division
        # EXCEPTION (not an abort — the cart's handler runs); INT_MIN/-1 is
        # special-cased (OV=1, avoids C UB); r30 (remainder) is written
        # FIRST, dest (quotient) LAST so dest wins when dest==r30; Z/S from
        # the dest's final value. cpu->pc is already this DIV's address, so
        # vb_exception saves it as EIPC (matching the oracle's RB_DECPCBY2).
        body = (f"int32_t _a=(int32_t)cpu->gpr[{r2}], _b=(int32_t)cpu->gpr[{r1}]; "
                "if (_b == 0) { vb_exception(cpu, VB_ZERO_DIV_HANDLER, "
                "VB_ECODE_ZERO_DIV); return; } "
                "int32_t _q,_r; "
                "if (_a == INT32_MIN && _b == -1) { _q=INT32_MIN; _r=0; "
                "  cpu->psw_ov=1; } else { _q=_a/_b; _r=_a%_b; "
                "  cpu->psw_ov=0; } "
                "cpu->gpr[30]=(uint32_t)_r; "
                + (f"cpu->gpr[{r2}]=(uint32_t)_q; " if write_dest else "")
                + f"cpu->psw_z=(cpu->gpr[{r2}]==0); "
                  f"cpu->psw_s=(cpu->gpr[{r2}]>>31)&1;")
        return "{ " + body + " }"
    if op == 0x0B:  # DIVU — unsigned
        # Same ÷0 exception and r30-first / dest-last order as DIV
        # (v810_oploop.inc:835-856).
        body = (f"uint32_t _a=cpu->gpr[{r2}], _b=cpu->gpr[{r1}]; "
                "if (_b == 0) { vb_exception(cpu, VB_ZERO_DIV_HANDLER, "
                "VB_ECODE_ZERO_DIV); return; } "
                "uint32_t _q=_a/_b, _r=_a%_b; "
                "cpu->gpr[30]=_r; "
                + (f"cpu->gpr[{r2}]=_q; " if write_dest else "")
                + f"cpu->psw_z=(cpu->gpr[{r2}]==0); "
                  f"cpu->psw_s=(cpu->gpr[{r2}]>>31)&1; cpu->psw_ov=0;")
        return "{ " + body + " }"
    if op in (0x0C, 0x0D, 0x0E):
        # OR / AND / XOR — reg2 ← reg2 OP reg1; Z/S updated, CY/OV unchanged.
        c_op = {0x0C: "|", 0x0D: "&", 0x0E: "^"}[op]
        body = (f"uint32_t _r=cpu->gpr[{r2}]{c_op}cpu->gpr[{r1}]; "
                + (f"cpu->gpr[{r2}]=_r; " if write_dest else "")
                + "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                  "cpu->psw_ov=0;")
        return "{ " + body + " }"
    if op == 0x0F:  # NOT — reg2 ← ~reg1
        body = (f"uint32_t _r=~cpu->gpr[{r1}]; "
                + (f"cpu->gpr[{r2}]=_r; " if write_dest else "")
                + "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                  "cpu->psw_ov=0;")
        return "{ " + body + " }"
    return (f"vb_stub_abort_simple(\"Format I opcode 0x{op:02X} not lifted\", "
            f"0x{ins.pc:08X}u);")


def _emit_format_ii(ins: DecodedInstruction) -> str:
    """Format II — register + 5-bit immediate (2 bytes)."""
    r2 = ins.reg2
    imm5_s = ins.imm5_s
    imm5_u = ins.imm5
    op = ins.opcode6
    write_dest = (r2 != 0)
    if op == 0x10:  # MOV imm5, r2 — reg2 ← sext(imm5); no PSW.
        if not write_dest:
            return ""
        return f"cpu->gpr[{r2}] = (uint32_t){imm5_s};"
    if op == 0x11:  # ADD imm5
        body = (f"uint32_t _a=cpu->gpr[{r2}]; uint32_t _b=(uint32_t){imm5_s}; "
                "uint32_t _r=_a+_b; "
                + (f"cpu->gpr[{r2}]=_r; " if write_dest else "")
                + "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                  "cpu->psw_cy=(_r<_a); "
                  "cpu->psw_ov=(((~(_a^_b))&(_a^_r))>>31)&1;")
        return "{ " + body + " }"
    if op == 0x12:  # SETF imm5, r2 — reg2 ← (cond imm5 met) ? 1 : 0
        cond = imm5_u & 0xF
        expr = _BCOND_C_EXPR.get(cond, "0")
        if not write_dest:
            return ""
        return f"cpu->gpr[{r2}] = ({expr}) ? 1u : 0u;"
    if op == 0x13:  # CMP imm5 — read-only
        body = (f"uint32_t _a=cpu->gpr[{r2}]; uint32_t _b=(uint32_t){imm5_s}; "
                "uint32_t _r=_a-_b; "
                "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                "cpu->psw_cy=(_a<_b); "
                "cpu->psw_ov=(((_a^_b)&(_a^_r))>>31)&1;")
        return "{ " + body + " }"
    if op in (0x14, 0x15, 0x17):  # SHL/SHR/SAR imm5
        # Shift count is imm5 zero-extended.
        sh = imm5_u
        if op == 0x14:  # SHL
            body = (f"uint32_t _a=cpu->gpr[{r2}]; uint32_t _sh={sh}; "
                    "uint32_t _r=_a<<_sh; "
                    "cpu->psw_cy = _sh ? ((_a>>(32-_sh))&1) : 0; "
                    + (f"cpu->gpr[{r2}]=_r; " if write_dest else "")
                    + "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                      "cpu->psw_ov=0;")
        else:
            cast = "(int32_t)" if op == 0x17 else ""
            body = (f"uint32_t _a=cpu->gpr[{r2}]; uint32_t _sh={sh}; "
                    f"uint32_t _r=(uint32_t)({cast}_a>>_sh); "
                    "cpu->psw_cy = _sh ? ((_a>>(_sh-1))&1) : 0; "
                    + (f"cpu->gpr[{r2}]=_r; " if write_dest else "")
                    + "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                      "cpu->psw_ov=0;")
        return "{ " + body + " }"
    if op == 0x16:  # CLI — clear interrupt disable
        return "cpu->psw_id = 0; cpu->cycle_deadline = cpu->cycles;"
    if op == 0x18:  # TRAP — software exception, vector imm5
        # vb_trap sets cpu->pc to the TRAP handler vector. Return
        # immediately so the dispatch loop reroutes to the new PC.
        return f"cpu->pc += 2; vb_trap(cpu, {imm5_u}u); return;"
    if op == 0x19:  # RETI
        # vb_reti restores cpu->pc + cpu->psw_* from EI*/FE*. Return
        # so the dispatch loop reroutes; pairs with TRAP/IRQ entry.
        return "vb_reti(cpu); return;"
    if op == 0x1A:  # HALT
        return ("/* HALT — recompiled cart is waiting for an interrupt. */ "
                "cpu->pc += 2; cpu->halted = 1; return;")
    if op == 0x1C:  # LDSR reg2, sysreg(imm5) — sysreg[imm5] ← reg2
        # Beetle V810::SetSREG semantics (v810_cpu.cpp:407-453).
        if imm5_u == 5:  # PSW — must keep exploded psw_* in sync
            return f"vb_psw_unpack(cpu, cpu->gpr[{r2}]); cpu->cycle_deadline = cpu->cycles;"
        if imm5_u in (0, 2, 25):  # EIPC / FEPC / ADTRE — even-aligned
            return (f"cpu->sysreg[{imm5_u}] = cpu->gpr[{r2}] "
                    f"& 0xFFFFFFFEu;")
        if imm5_u in (1, 3):  # EIPSW / FEPSW — valid-PSW-bits mask
            return (f"cpu->sysreg[{imm5_u}] = cpu->gpr[{r2}] "
                    f"& 0x000FF3FFu;")
        if imm5_u in (4, 6, 7):  # ECR / PIR / TKCW — read-only
            return ""
        if imm5_u == 24:  # CHCW — no I-cache modelling (transparent)
            return ""
        return (f"vb_stub_abort_simple(\"LDSR to reserved sysreg "
                f"{imm5_u}\", 0x{ins.pc:08X}u);")
    if op == 0x1D:  # STSR sysreg(imm5), reg2 — reg2 ← sysreg[imm5]
        if not write_dest:
            return ""
        if imm5_u == 5:  # PSW — repack exploded form to canonical 32-bit
            return f"cpu->gpr[{r2}] = vb_psw_pack(cpu);"
        if imm5_u in (0, 1, 2, 3, 4, 6, 7, 24, 25):
            return f"cpu->gpr[{r2}] = cpu->sysreg[{imm5_u}];"
        return (f"vb_stub_abort_simple(\"STSR from reserved sysreg "
                f"{imm5_u}\", 0x{ins.pc:08X}u);")
    if op == 0x1E:  # SEI — set interrupt disable
        return "cpu->psw_id = 1;"
    return (f"vb_stub_abort_simple(\"Format II opcode 0x{op:02X} not lifted\", "
            f"0x{ins.pc:08X}u);")


def _emit_format_v(ins: DecodedInstruction) -> str:
    """Format V — three-operand imm16 arithmetic (4 bytes)."""
    r1, r2, imm = ins.reg1, ins.reg2, ins.imm16
    imm_s = ins.imm16_s
    op = ins.opcode6
    write_dest = (r2 != 0)
    if not write_dest and op in (0x28, 0x2F):
        return ""
    if op == 0x28:  # MOVEA — reg2 ← reg1 + sext(imm16); no PSW
        return (f"cpu->gpr[{r2}] = cpu->gpr[{r1}] + (uint32_t)({imm_s});")
    if op == 0x29:  # ADDI — reg2 ← reg1 + sext(imm16); PSW updated
        body = (f"uint32_t _a=cpu->gpr[{r1}]; uint32_t _b=(uint32_t){imm_s}; "
                "uint32_t _r=_a+_b; "
                f"cpu->gpr[{r2}]=_r; "
                "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                "cpu->psw_cy=(_r<_a); "
                "cpu->psw_ov=(((~(_a^_b))&(_a^_r))>>31)&1;")
        return "{ " + body + " }"
    if op == 0x2C:  # ORI — reg2 ← reg1 | zext(imm16); PSW Z/S, CY/OV cleared
        body = (f"uint32_t _r=cpu->gpr[{r1}]|0x{imm:04X}u; "
                f"cpu->gpr[{r2}]=_r; "
                "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                "cpu->psw_ov=0;")
        return "{ " + body + " }"
    if op == 0x2D:  # ANDI — zext immediate
        body = (f"uint32_t _r=cpu->gpr[{r1}]&0x{imm:04X}u; "
                f"cpu->gpr[{r2}]=_r; "
                "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                "cpu->psw_ov=0;")
        return "{ " + body + " }"
    if op == 0x2E:  # XORI
        body = (f"uint32_t _r=cpu->gpr[{r1}]^0x{imm:04X}u; "
                f"cpu->gpr[{r2}]=_r; "
                "cpu->psw_z=(_r==0); cpu->psw_s=(_r>>31)&1; "
                "cpu->psw_ov=0;")
        return "{ " + body + " }"
    if op == 0x2F:  # MOVHI — reg2 ← reg1 + (imm16 << 16); no PSW
        return (f"cpu->gpr[{r2}] = cpu->gpr[{r1}] + (uint32_t)(0x{imm:04X}u << 16);")
    return (f"vb_stub_abort_simple(\"Format V opcode 0x{op:02X} not lifted\", "
            f"0x{ins.pc:08X}u);")


def _emit_format_vi(ins: DecodedInstruction) -> str:
    """Format VI — load/store/IN/OUT/CAXI (4 bytes; disp16(reg1))."""
    r1, r2 = ins.reg1, ins.reg2
    disp = ins.imm16_s
    op = ins.opcode6
    write_dest = (r2 != 0)
    addr_expr = f"(cpu->gpr[{r1}] + (uint32_t)({disp}))"

    # Loads — IN.* is treated identically to LD.* per Sacred Tech Scroll.
    if op == 0x30:  # LD.B / IN.B — sign-extend the byte
        if not write_dest:
            return f"(void)cpu->read8({addr_expr});"
        return (f"cpu->gpr[{r2}] = (uint32_t)(int32_t)(int8_t)"
                f"cpu->read8({addr_expr});")
    if op == 0x31:  # LD.H / IN.H — sign-extend the halfword
        if not write_dest:
            return f"(void)cpu->read16({addr_expr});"
        return (f"cpu->gpr[{r2}] = (uint32_t)(int32_t)(int16_t)"
                f"cpu->read16({addr_expr});")
    if op in (0x33, 0x3B):  # LD.W / IN.W — 32-bit word, no extension
        if not write_dest:
            return f"(void)cpu->read32({addr_expr});"
        return f"cpu->gpr[{r2}] = cpu->read32({addr_expr});"

    if op in (0x38, 0x39):
        width = 8 if op == 0x38 else 16
        return f"cpu->gpr[{r2}] = cpu->read{width}({addr_expr});" if write_dest else f"(void)cpu->read{width}({addr_expr});"

    # Stores
    if op in (0x34, 0x3C):  # ST.B / OUT.B
        return f"cpu->write8({addr_expr}, (uint8_t)cpu->gpr[{r2}]);"
    if op in (0x35, 0x3D):  # ST.H / OUT.H
        return f"cpu->write16({addr_expr}, (uint16_t)cpu->gpr[{r2}]);"
    if op in (0x37, 0x3F):  # ST.W / OUT.W
        return f"cpu->write32({addr_expr}, cpu->gpr[{r2}]);"

    if op == 0x3A:  # CAXI — compare-and-exchange (P4+, mutex code)
        return (f"{{ uint32_t _addr={addr_expr}&~3u, _old=cpu->read32(_addr), _a=cpu->gpr[{r2}], _r=_a-_old; "
                "cpu->psw_z=(_r==0); cpu->psw_s=_r>>31; cpu->psw_cy=_a<_old; "
                "cpu->psw_ov=((_a^_old)&(_a^_r))>>31; "
                "cpu->write32(_addr,_r==0 ? cpu->gpr[30]:_old); "
                + (f"cpu->gpr[{r2}]=_old; " if write_dest else "") + "}")

    return (f"vb_stub_abort_simple(\"Format VI opcode 0x{op:02X} not lifted\", "
            f"0x{ins.pc:08X}u);")


def _emit_format_vii(ins: DecodedInstruction) -> str:
    """Format VII — FPP / extended (primary 0x3E) and BSU (primary 0x1F).
    FPP sub-ops follow Beetle's v810_cpu.cpp fpu_subop (lines 941-1107):
    arg1/arg2 in Beetle's notation map to OUR reg2/reg1 (Beetle decodes
    Format VII as arg1=bits 9:5 / arg2=bits 4:0; our decoder uses the
    Format I convention reg1=bits 4:0 / reg2=bits 9:5).

    Flag policy:
      ADDF.S / SUBF.S / MULF.S / DIVF.S → SetFPUOPNonFPUFlags:
        OV = 0
        result==±0 → Z=1, S=0, CY=0
        else       → Z=0, S = sign bit, CY = sign bit
      CMPF.S → equal: Z=1 S=0 CY=0; less: S=1 CY=1; greater: S=0 CY=0; OV=0
      CVT.SW / TRNC.SW → SetSZ on the int32 result; OV=0
      CVT.WS → SetFPUOPNonFPUFlags on the float bits
      XB / XH / REV / MPYHW → no PSW change

    The FRO / FIV / FZD / FOV / FUD / FPR exception machinery is NOT
    modeled — the cart code that uses these ops is expected to feed
    normal (non-subnormal, non-NaN, non-Inf) inputs. If a future cart
    needs the exception path, plumb it through here.
    """
    first = int.from_bytes(ins.raw[:2], "little")
    second = int.from_bytes(ins.raw[2:4], "little") if len(ins.raw) >= 4 else 0
    return f"vb_interpreter_extended(cpu, 0x{first:04X}u, 0x{second:04X}u); cpu->gpr[0]=0; return;"


def _emit_straight_line(ins: DecodedInstruction) -> str:
    """Emit the body of one instruction that is NOT a control transfer.
    Control flow is handled by the block emitter."""
    if ins.is_unknown:
        return (f"vb_stub_abort_simple(\"unknown encoding raw={ins.raw.hex()} "
                f"({ins.notes})\", 0x{ins.pc:08X}u);")
    if ins.fmt is Format.I:
        return _emit_format_i(ins)
    if ins.fmt is Format.II:
        return _emit_format_ii(ins)
    if ins.fmt is Format.V:
        return _emit_format_v(ins).replace("cpu->gpr[0]=_r; ", "")
    if ins.fmt is Format.VI:
        return _emit_format_vi(ins)
    if ins.fmt is Format.VII:
        return _emit_format_vii(ins)
    # Format III (Bcond), Format IV (JR/JAL) are control flow.
    return (f"vb_stub_abort_simple(\"control-flow op {ins.mnemonic} fell "
            f"through to straight-line emit\", 0x{ins.pc:08X}u);")


# ---------- Control-flow / terminator emit ----------

def _emit_terminator(ins: DecodedInstruction,
                     fn: FunctionRange,
                     leader_set: Set[int],
                     fn_entries: Set[int]) -> List[str]:
    """Emit C for a control-flow-terminating instruction.

    `leader_set` is the set of basic-block start PCs *within fn* — used
    to decide whether a branch target gets a local goto or a dispatch.
    `fn_entries` is the set of all recompiled function entry PCs —
    used to keep tail-calls to other recompiled functions on the
    fast path.
    """
    out: List[str] = []
    fall_through_pc = ins.pc + ins.size

    # Calling-convention notes
    # ------------------------
    # NON-CALL transfers (JR, JMP rX, Bcond-leaving-fn, fall-through)
    # are loop-style: set cpu->pc to the new target and `return`. The
    # outer trampoline in vb_dispatch loops over the new pc and routes
    # to the next vb_fn_<pc> without growing the C call stack. This
    # is the classic "interpreter inner loop" pattern; we use it
    # because Mario's Tennis's WRAM-init loop would otherwise blow the
    # 1 MB Windows stack within a few hundred iterations.
    #
    # JAL stays as a recursive C call (vb_dispatch_call) — the call
    # depth on real V810 code is bounded by hardware-stack depth, so
    # the native C stack mirrors it cleanly. JMP r31 returns from
    # the current recompiled function, popping that frame.

    if ins.fmt is Format.III and ins.mnemonic != "NOP":
        cond_expr = _BCOND_C_EXPR[ins.cond]
        target = ins.branch_target
        out.append(f"if ({cond_expr}) {{")
        # Axis-2: a taken conditional branch costs BCOND_TAKEN_EXTRA more
        # than the not-taken base (1) already charged inline (oracle
        # v810_oploop.inc:420 taken=3 / :430 not-taken=1).
        out.append(f"    cpu->cycles += {BCOND_TAKEN_EXTRA};")
        if target in leader_set:
            out.append(f"    goto {_bb_label(target)};")
        else:
            out.append(f"    cpu->pc = 0x{target & 0xFFFFFFFF:08X}u;")
            out.append(f"    return;")
        out.append(f"}}")
        return out

    if ins.fmt is Format.III and ins.mnemonic == "NOP":
        return []

    if ins.fmt is Format.IV and ins.opcode6 == 0x2A:  # JR
        target = ins.branch_target & 0xFFFFFFFF
        if target in leader_set:
            out.append(f"goto {_bb_label(target)};")
        else:
            out.append(f"cpu->pc = 0x{target:08X}u;")
            out.append(f"return;")
        return out

    if ins.fmt is Format.IV and ins.opcode6 == 0x2B:  # JAL
        target = ins.branch_target & 0xFFFFFFFF
        out.append(f"cpu->pc = 0x{fall_through_pc & 0xFFFFFFFF:08X}u;")
        out.append(f"vb_dispatch_call(cpu, 0x{target:08X}u, "
                   f"0x{fall_through_pc & 0xFFFFFFFF:08X}u);")
        # If the inner dispatch yielded (step budget exhausted) or
        # halted, bubble up immediately so the main loop can poll
        # TCP. cpu->pc is already set to the right resume point —
        # either pc_after_jal (clean JMP r31 return) or wherever the
        # inner dispatch left off.
        out.append("if (cpu->yielded || cpu->halted) return;")
        return out

    if ins.fmt is Format.I and ins.opcode6 == 0x06:  # JMP rX
        # JMP r31 is the canonical return — set cpu->pc = gpr[31] so
        # the outer vb_dispatch_call's `cpu->pc == saved_lp` check
        # can detect a clean return. JMP rX (X != 31) sets cpu->pc to
        # the new target and lets the outer loop dispatch.
        out.append(f"cpu->pc = cpu->gpr[{ins.reg1}] & 0xFFFFFFFEu;")
        out.append(f"return;")
        return out

    # Other terminators (HALT/RETI/TRAP) already emit `return` via the
    # straight-line path.
    return []


# ---------- Block + function emit ----------

@dataclass
class _LeaderInfo:
    """All the per-leader bookkeeping the function emitter needs."""
    leader_pcs: Set[int]


def _collect_leaders(rom: RomImage, fn: FunctionRange) -> Set[int]:
    """Re-derive leader PCs by walking the function. We could use
    `build_cfg(fn, rom).blocks` here but that walks the same data —
    keeping a dedicated walker keeps the emitter self-contained for
    testing.

    A leader is added only when it is strictly inside [start_pc, end_pc).
    The first-found-out-of-range bug: when JAL's fall-through PC equals
    fn.end_pc, adding it to leaders made the emitter believe a JR
    elsewhere in the function could goto that PC — but the function-
    body emit loop stops at end_pc (exclusive), so the label was never
    defined and gcc rejected the goto.
    """
    leaders: Set[int] = {fn.start_pc}
    work: List[int] = [fn.start_pc]
    visited: Set[int] = set()

    def in_range(p: int) -> bool:
        return fn.start_pc <= p < fn.end_pc

    while work:
        cur = work.pop()
        while True:
            if cur in visited or not in_range(cur):
                break
            ins = rom.decode_at_va(cur)
            if ins is None or ins.is_unknown:
                visited.add(cur); break
            visited.add(cur)
            nxt = cur + ins.size
            if ins.fmt is Format.III and ins.mnemonic != "NOP":
                tgt = ins.branch_target
                if tgt is not None and in_range(tgt):
                    leaders.add(tgt); work.append(tgt)
                if in_range(nxt):
                    leaders.add(nxt)
                cur = nxt; continue
            if ins.fmt is Format.IV and ins.opcode6 == 0x2A:  # JR
                if (ins.branch_target is not None
                        and in_range(ins.branch_target)):
                    leaders.add(ins.branch_target)
                    cur = ins.branch_target; continue
                break
            if ins.fmt is Format.IV and ins.opcode6 == 0x2B:  # JAL
                if in_range(nxt):
                    leaders.add(nxt)
                cur = nxt; continue
            if ins.fmt is Format.I and ins.opcode6 == 0x06:
                break
            if (ins.fmt is Format.II
                    and ins.opcode6 in (0x18, 0x19, 0x1A)):
                break  # TRAP / RETI / HALT
            cur = nxt

    # Filter leaders to those the linear emit pass will actually land on.
    # `emit_function` iterates pc = fn.start_pc, advancing by ins.size of
    # whatever decode_at_va returns. When the function range covers data
    # walked from a toml-seeded false positive, the CFG walk above may
    # land on PCs that the linear iteration skips over (because adjacent
    # bytes happened to decode as a 4-byte ins in linear order vs two
    # 2-byte ins from the CFG entry). Without this filter the emitter
    # generates `goto bb_<unreachable-pc>` that gcc rejects with
    # "label used but not defined".
    linear_pcs: Set[int] = set()
    lp = fn.start_pc
    while lp < fn.end_pc:
        linear_pcs.add(lp)
        ins = rom.decode_at_va(lp)
        if ins is None:
            break
        lp += max(ins.size, 2)
    # Every instruction is a legal resume point after a device deadline,
    # debugger step, exception return, or interpreter/native handoff.
    return linear_pcs


def emit_function(rom: RomImage, fn: FunctionRange,
                  fn_entries: Set[int]) -> str:
    """Emit the C source of one recompiled function.

    The function body opens with a routing switch over every basic-
    block leader so dispatch can land at any leader, not just the
    function head. This is the resume path for two situations:
      * a yield (step-budget exhausted mid-function) returns with
        cpu->pc set to a leader; the next dispatch needs to land
        exactly there;
      * a Bcond/JR from another function targets a mid-function
        leader (the cross-function-target promoter handles most of
        these but the catch-all here makes the layout robust).
    """
    leaders = _collect_leaders(rom, fn)
    sorted_leaders = sorted(leaders)
    pc = fn.start_pc

    lines: List[str] = []
    lines.append(f"/* {_fn_symbol(fn.start_pc)}: "
                 f"0x{fn.start_pc:08X}..0x{fn.end_pc:08X} */")
    lines.append(f"void {_fn_symbol(fn.start_pc)}(CPUState* cpu) {{")
    # Routing switch — only emit if there's more than one leader.
    # (For single-leader functions the entry is unambiguous.)
    if sorted_leaders:
        lines.append(f"    switch (cpu->pc) {{")
        for leader in sorted_leaders:
            if leader == fn.start_pc:
                continue   # fall through to bb_<start_pc> by default
            lines.append(f"    case 0x{leader & 0xFFFFFFFF:08X}u: "
                         f"goto {_bb_label(leader)};")
        lines.append(f"    case 0x{fn.start_pc:08X}u: break;")
        lines.append("    default: vb_interpreter_fallback(cpu); return;")
        lines.append(f"    }}")
    # Always-on fntrace: record fresh-call entry. Multi-leader fns reach
    # this line only when the leader-routing switch took the `default`
    # branch (i.e. fresh JAL or unknown-pc fallback); resumed mid-fn
    # entries jump into a case branch and skip the record. Single-leader
    # fns record every entry; downstream analysis can collapse repeat-
    # same-pc runs if needed.
    lines.append(f"    vb_fntrace_record(0x{fn.start_pc:08X}u, "
                 f"cpu->gpr[31]);")
    lines.append(f"    cpu->pc = 0x{fn.start_pc:08X}u;")

    while pc < fn.end_pc:
        if pc in leaders:
            lines.append(f"{_bb_label(pc)}:;")
            # Every instruction PC is a leader so deadlines, debugger stops,
            # and fallback handoffs resume without repeating guest effects.
            lines.append(f"    if (cpu->step_budget == 0 "
                         f"|| cpu->cycles >= cpu->cycle_deadline) "
                         f"{{ cpu->yielded = 1; "
                         f"cpu->pc = 0x{pc & 0xFFFFFFFF:08X}u; return; }}")
            lines.append(f"    cpu->step_budget--;")
        ins = rom.decode_at_va(pc)
        if ins is None:
            lines.append(f"    /* fell off cart at 0x{pc:08X} */")
            lines.append(f"    vb_stub_abort_simple(\"fell off cart\", "
                         f"0x{pc:08X}u);")
            break

        lines.append(f"    cpu->pc = 0x{ins.pc:08X}u;")
        lines.append("    if (vb_execution_before(cpu)) return;")
        lines.append("    ++vb_execution.native_instructions;")

        # Per-instruction CPU-hook (Axis 1/2/3/6 divergence harness). No-op
        # unless built -DVB_CPUHOOK_ENABLE. Emitted BEFORE the cycle charge
        # and body so it records pre-instruction {pc,regs,psw,cycle},
        # matching the oracle's RB_CPUHOOK (which fires before execution).
        lines.append(f"    VB_CPUHOOK(cpu);")

        # Axis-2 cycle model: charge this instruction's V810 base cost
        # (single source: cycles.instr_base_cycles, from the oracle's
        # ADDCLOCK table). Charged BEFORE the body/terminator so it lands
        # even for control transfers that leave the block. For a Bcond
        # this is the not-taken base (1); the taken extra is added inside
        # the taken path by _emit_terminator. Replaces main.cpp's old
        # bbs_run*3 estimate, which is now derived as the cpu->cycles delta.
        lines.append(f"    vb_execution_charge(cpu, 0x{ins.opcode6:02X}, {instr_base_cycles(ins)});")

        # Straight-line body (if any).
        body = _emit_straight_line(ins) if (
            ins.fmt not in (Format.III, Format.IV)
            and not (ins.fmt is Format.I and ins.opcode6 == 0x06)
        ) else ""
        if body:
            lines.append(f"    {body}")

        # Terminator handling.
        if (ins.fmt is Format.III and ins.mnemonic != "NOP") or \
           (ins.fmt is Format.IV) or \
           (ins.fmt is Format.I and ins.opcode6 == 0x06):
            term_lines = _emit_terminator(ins, fn, leaders, fn_entries)
            for tl in term_lines:
                lines.append("    " + tl)
            # JR / JMP rX / JMP r31 / JAL-with-link-but-final-return
            # leave the block without fall-through; if the next pc is
            # NOT a leader, the function effectively ends here.
            if (ins.fmt is Format.IV and ins.opcode6 == 0x2A) or \
               (ins.fmt is Format.I and ins.opcode6 == 0x06):
                # Hard terminator — close out the current path. The
                # decoder will continue past here only if the
                # following PC is also a leader (reached by another
                # branch).
                pass

        pc = pc + ins.size

    # End of the function's byte range without an explicit terminator —
    # the cart's code fell straight through to whatever the next
    # function is. Loop-style: set cpu->pc to fn.end_pc and return; the
    # outer trampoline picks up the next function.
    lines.append(f"    cpu->pc = 0x{fn.end_pc & 0xFFFFFFFF:08X}u;")
    lines.append(f"    return;")
    lines.append(f"}}")
    return "\n".join(lines)


# ---------- Module-level emit ----------

@dataclass
class CodegenResult:
    """Files the codegen produced, keyed by relative path."""
    files: Dict[str, str]
    function_count: int
    instruction_count: int


def emit_full_c(rom: RomImage, fns: List[FunctionRange],
                module_name: str) -> str:
    """Render `generated/<module>_full.c` — every function body."""
    parts: List[str] = []
    parts.append(f"/* AUTOGENERATED by recompiler/v810/emitter.py. "
                 f"DO NOT EDIT — Rule 4. */")
    parts.append(f"/* module: {module_name}    functions: {len(fns)} */")
    parts.append(f"#include <math.h>")
    parts.append(f"#include <stdint.h>")
    parts.append(f"#include \"cpu_state.h\"")
    parts.append(f"#include \"interrupts.h\"")
    parts.append(f"#include \"stub_abort.h\"")
    parts.append(f"#include \"fntrace.h\"")
    parts.append(f"#include \"{module_name}.h\"")
    parts.append('#include "v810_interpreter.h"')
    parts.append("")
    fn_entries = {fn.start_pc for fn in fns}
    for fn in fns:
        parts.append(emit_function(rom, fn, fn_entries))
        parts.append("")
    return "\n".join(parts)


def emit_dispatch_c(fns: List[FunctionRange], module_name: str,
                    rom_crc32: int = 0, rom: Optional[RomImage] = None) -> str:
    """Render `generated/<module>_dispatch.c` — vb_dispatch + vb_dispatch_call
    plus vb_game_expected_crc32() which main.cpp uses to refuse a ROM
    that doesn't match what the recompiler was generated against."""
    parts: List[str] = []
    parts.append(f"/* AUTOGENERATED by recompiler/v810/emitter.py. "
                 f"DO NOT EDIT — Rule 4. */")
    parts.append(f"/* module: {module_name}    entries: {len(fns)} */")
    parts.append('#include "v810_interpreter.h"')
    parts.append(f"#include <stdint.h>")
    parts.append(f"#include \"cpu_state.h\"")
    parts.append(f"#include \"interrupts.h\"")
    parts.append(f"#include \"stub_abort.h\"")
    parts.append(f"#include \"{module_name}.h\"")
    parts.append("")
    parts.append("/* Loop-style dispatch. vb_dispatch_call repeatedly invokes")
    parts.append(" * vb_fn_<cpu->pc> until the callee returns to its caller's")
    parts.append(" * expected return PC (clean JMP r31 unwind, signalled by")
    parts.append(" * cpu->pc == saved_lp) or the CPU halts. Any JR / JMP rX /")
    parts.append(" * fall-through inside a function sets cpu->pc and returns,")
    parts.append(" * looping here without growing the C stack — the pattern")
    parts.append(" * that lets the WRAM-init loop (~32k iterations on Mario's")
    parts.append(" * Tennis) run without blowing the native stack.")
    parts.append(" *")
    parts.append(" * JAL stays as a recursive call (this function calls itself")
    parts.append(" * via the recompiled body's vb_dispatch_call invocation);")
    parts.append(" * the native C call depth mirrors hardware call depth.")
    parts.append(" *")
    parts.append(" * Top-level dispatch has no guest return address. The uint64_t")
    parts.append(" * stop value lies outside the V810 address space and is never")
    parts.append(" * written into guest registers. */")
    parts.append("#define VB_TOP_LEVEL_LP UINT64_MAX")
    routes = {}
    for fn in sorted(fns, key=lambda f: f.start_pc):
        pcs = _collect_leaders(rom, fn) if rom is not None else {fn.start_pc}
        for pc in pcs:
            routes[pc] = fn.start_pc
    parts.append("typedef void (*VbNativeFn)(CPUState*);")
    parts.append("static const struct { uint32_t pc; VbNativeFn fn; } vb_routes[] = {")
    for pc, entry in sorted(routes.items()):
        parts.append(f"    {{0x{pc:08X}u, {_fn_symbol(entry)}}},")
    parts.append("};")
    parts.append("static VbNativeFn vb_native_lookup(uint32_t pc) {")
    parts.append("    unsigned lo=0, hi=sizeof(vb_routes)/sizeof(vb_routes[0]);")
    parts.append("    while (lo < hi) { unsigned mid=lo+(hi-lo)/2; if(vb_routes[mid].pc < pc) lo=mid+1; else hi=mid; }")
    parts.append("    return lo < sizeof(vb_routes)/sizeof(vb_routes[0]) && vb_routes[lo].pc == pc ? vb_routes[lo].fn : 0;")
    parts.append("}")
    parts.append("static void vb_dispatch_loop(CPUState* cpu, uint64_t saved_lp) {")
    parts.append("    /* NOTE: do NOT clear cpu->yielded here — the main")
    parts.append("     * loop clears it before the top-level vb_dispatch. A")
    parts.append("     * recursive vb_dispatch_call from inside a yielded")
    parts.append("     * stack must preserve the flag so the unwind can")
    parts.append("     * bubble it back to the top. */")
    parts.append("    while (cpu->pc != saved_lp && !cpu->halted "
                 "&& !cpu->yielded) {")
    parts.append("        uint32_t pc = cpu->pc;")
    parts.append("        if (vb_execution.mode == VB_EXEC_INTERPRETER) {")
    parts.append("            if (!cpu->step_budget || cpu->cycles >= cpu->cycle_deadline) { cpu->yielded = 1; return; }")
    parts.append("            --cpu->step_budget; vb_interpreter_step(cpu); continue;")
    parts.append("        }")
    parts.append("        VbNativeFn native = vb_native_lookup(pc);")
    parts.append("        if (native) { ++vb_execution.native_entries; native(cpu); }")
    parts.append("        else { vb_interpreter_fallback(cpu); }")
    parts.append("    }")
    parts.append("}")
    parts.append("")
    parts.append("void vb_dispatch_call(CPUState* cpu, uint32_t target_pc, "
                 "uint32_t lp) {")
    parts.append("    /* JAL semantic: writeback the link register and enter")
    parts.append("     * the dispatch loop with the JAL's fall-through PC as")
    parts.append("     * the saved_lp gate. The callee's eventual JMP r31")
    parts.append("     * returns control here when cpu->pc == lp. */")
    parts.append("    cpu->gpr[31] = lp;")
    parts.append("    cpu->pc = target_pc;")
    parts.append("    vb_dispatch_loop(cpu, lp);")
    parts.append("}")
    parts.append("")
    parts.append("void vb_dispatch(CPUState* cpu, uint32_t target_pc) {")
    parts.append("    /* Preserve the live link register on initial entry and resume. */")
    parts.append("    cpu->pc = target_pc;")
    parts.append("    vb_dispatch_loop(cpu, VB_TOP_LEVEL_LP);")
    parts.append("}")
    parts.append("")
    parts.append("/* CRC32 of the ROM this recompiler run was generated from.")
    parts.append(" * main.cpp computes the CRC32 of the file it loads and compares;")
    parts.append(" * a mismatch means the user provided a different cart than the")
    parts.append(" * one this binary was built for. */")
    parts.append(f"uint32_t vb_game_expected_crc32(void) "
                 f"{{ return 0x{rom_crc32 & 0xFFFFFFFF:08X}u; }}")
    return "\n".join(parts)


def emit_header(fns: List[FunctionRange], module_name: str) -> str:
    """Render `generated/<module>.h` — vb_fn_<va> prototypes."""
    parts: List[str] = []
    parts.append(f"/* AUTOGENERATED by recompiler/v810/emitter.py. "
                 f"DO NOT EDIT — Rule 4. */")
    parts.append(f"#ifndef VB_GENERATED_{module_name.upper()}_H")
    parts.append(f"#define VB_GENERATED_{module_name.upper()}_H")
    parts.append(f"#include \"cpu_state.h\"")
    parts.append("#ifdef __cplusplus")
    parts.append("extern \"C\" {")
    parts.append("#endif")
    parts.append("")
    for fn in sorted(fns, key=lambda f: f.start_pc):
        parts.append(f"void {_fn_symbol(fn.start_pc)}(CPUState* cpu);")
    parts.append("")
    parts.append("#ifdef __cplusplus")
    parts.append("}")
    parts.append("#endif")
    parts.append("#endif")
    return "\n".join(parts)


def recompile_rom(rom: RomImage, *, module_name: str = "cart",
                  function_limit: Optional[int] = None,
                  extra_seeds: Optional[Iterable[int]] = None) -> CodegenResult:
    """Top-level driver: discover functions, recompile up to
    `function_limit` of them (BFS-from-entry order), produce the
    three generated files.

    `extra_seeds` is an optional list of additional function-entry
    PCs to seed the CFG walker with. Pull these from the per-cart
    `[functions]` toml when a function-pointer-table entry is
    invisible to the static heuristic walker.

    Functions outside the limit are not emitted; calls to them via
    JAL/JR will hit `vb_dispatch`'s default arm and abort cleanly,
    surfacing the gap (per CLAUDE.md §0 no-stub policy)."""
    fns_all = discover_functions(rom, extra_seeds=extra_seeds)
    if not fns_all:
        return CodegenResult(files={}, function_count=0, instruction_count=0)

    if function_limit is not None and function_limit < len(fns_all):
        fns = fns_all[:function_limit]
    else:
        fns = fns_all

    insn_count = 0
    for fn in fns:
        pc = fn.start_pc
        while pc < fn.end_pc:
            ins = rom.decode_at_va(pc)
            if ins is None:
                break
            insn_count += 1
            pc += ins.size

    import zlib
    rom_crc32 = zlib.crc32(rom.data) & 0xFFFFFFFF
    files = {
        f"generated/{module_name}_full.c": emit_full_c(rom, fns, module_name),
        f"generated/{module_name}_dispatch.c":
            emit_dispatch_c(fns, module_name, rom_crc32=rom_crc32, rom=rom),
        f"generated/{module_name}.h": emit_header(fns, module_name),
    }
    return CodegenResult(files=files, function_count=len(fns),
                         instruction_count=insn_count)
