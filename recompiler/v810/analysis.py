"""V810 function discovery + CFG-aware decoding.

A linear scan over a VB cart's ROM hits ~3% unknown encodings on
Mario's Tennis because the scan walks into post-branch data (jump
tables, constant pools, padding). The CFG-aware walk visits only
instructions actually reachable from the reset trampoline + every
transitively-reached JAL target, and the residual unknowns evaporate.

Public surface:

  RomImage                  cart + mirror-aware VA-to-offset translation
  trace_reset_trampoline    resolve the cart entry PC from the thunk
                            at 0xFFFFFFF0 (MOVHI / MOVEA / JMP pattern)
  cfg_walk_from_seeds       BFS over CFG; returns visited PCs, JAL
                            targets, indirect jumps, and unknowns
  discover_functions        bucket visited PCs into FunctionRanges
  build_cfg                 per-function basic-block CFG

This file is the canonical entry point for P2 oracle-parity tests and
for P3's lifter (the lifter walks discover_functions() output, never
the linear scan).
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Set, Tuple

from .decoder import DecodedInstruction, decode_at
from .isa import Format


PHASE = "P2 — Function discovery + CFG"


# V810 reset vector. The hardware loads PC from this address on
# power-on. The cart's last 16 bytes are mirrored here.
RESET_VECTOR = 0xFFFFFFF0

# Cart-provided handler vectors (cart ROM top page; 16-byte slots).
#
# Interrupt entry: PC ← 0xFFFFFE00 | (level << 4). Only the 5 VBIRQ
# source levels are reachable on the Virtual Boy hardware; higher V810
# levels (5..15) exist in the manual but have no physical source on
# this platform, so we don't seed them.
#
# Exception entry: PC ← one of the fixed handlers below, set by
# vb_exception() for software faults (Beetle V810::Exception). The
# cart's ROM populates these with handler code when it cares (TRAP,
# divide-by-zero, invalid opcode). Pads of 0xFF stop the walker
# cleanly via the unknown-encoding path.
HANDLER_VECTORS = (
    # Hardware interrupt vectors (VBIRQ levels 0..4)
    0xFFFFFE00,  # INPUT
    0xFFFFFE10,  # TIMER
    0xFFFFFE20,  # EXPANSION
    0xFFFFFE30,  # COMM
    0xFFFFFE40,  # VIP
    # Software exception vectors (Beetle v810_cpu.h:33-37)
    0xFFFFFF60,  # FPU base    (FRO, FIV, FZD, FOV, FUD, FPR clustered)
    0xFFFFFF80,  # ZERO_DIV
    0xFFFFFF90,  # INVALID_OP
    0xFFFFFFA0,  # TRAP 0..15
    0xFFFFFFB0,  # TRAP 16..31
    0xFFFFFFD0,  # Double-fault
)


# V810 physical address mask. Bits 31:27 are ignored by the bus; only
# bits 26:0 reach memory.
PHYS_MASK = 0x07FFFFFF


# Cart ROM lives in bank 7. Banks 0..6 are WRAM / VIP / VSU / pad /
# expansion / cart-RAM / reserved.
CART_BANK_BASE = 0x07000000
CART_BANK_END = 0x08000000   # exclusive — bank 7 ends at 0x08000000


@dataclass(frozen=True)
class RomImage:
    """A loaded VB cart plus the rules for mapping VAs into its bytes.

    The cart is mirrored across bank 7 (0x07000000-0x07FFFFFF) of the
    V810 physical address space. For every commercial VB cart the size
    is a power of two and mirroring is exact-modulus; non-power-of-two
    carts are clamped (i.e. accesses past `rom_size` return None
    instead of wrapping).
    """
    data: bytes
    rom_size: int

    @classmethod
    def from_bytes(cls, data: bytes) -> "RomImage":
        return cls(data=data, rom_size=len(data))

    def va_to_offset(self, va: int) -> Optional[int]:
        """Translate `va` to a ROM byte offset; None if `va` is not
        inside the cart's bank-7 mirror."""
        phys = va & PHYS_MASK
        if phys < CART_BANK_BASE or phys >= CART_BANK_END:
            return None
        offset_in_bank = phys - CART_BANK_BASE
        if self.rom_size > 0 and (self.rom_size & (self.rom_size - 1)) == 0:
            return offset_in_bank & (self.rom_size - 1)
        if offset_in_bank < self.rom_size:
            return offset_in_bank
        return None

    def can_read_insn(self, va: int) -> bool:
        off = self.va_to_offset(va)
        return off is not None and off + 2 <= self.rom_size

    def decode_at_va(self, va: int) -> Optional[DecodedInstruction]:
        """Decode one instruction at `va`. Returns None only if `va`
        falls outside the cart mirror entirely; a truncated or
        reserved insn yields a `DecodedInstruction` with
        `is_unknown=True`."""
        off = self.va_to_offset(va)
        if off is None or off >= self.rom_size:
            return None
        return decode_at(self.data, off, pc=va)


def _is_no_write_insn(ins: DecodedInstruction) -> bool:
    """Does this insn leave every GPR unchanged?

    A reliable list is important to the constant tracker: an insn that
    actually writes reg2 must invalidate any cached constant for that
    register, while an insn that leaves it alone must preserve it.
    """
    if ins.fmt is Format.I and ins.opcode6 in (0x03, 0x06):
        return True  # CMP r1,r2 / JMP rX
    if ins.fmt is Format.II and ins.opcode6 in (
        0x13,  # CMP imm5
        0x18,  # TRAP
        0x19,  # RETI
        0x1A,  # HALT
        0x16,  # CLI
        0x1E,  # SEI
        0x1C,  # LDSR (writes sysreg, not GPR)
    ):
        return True
    if ins.fmt is Format.III:
        return True  # Bcond — no GPR write
    if ins.fmt is Format.IV and ins.opcode6 == 0x2A:
        return True  # JR — no link
    if ins.fmt is Format.VI and ins.is_store:
        return True
    if ins.fmt is Format.VI and ins.opcode6 in (0x3C, 0x3D, 0x3F):
        return True  # OUT.B/H/W
    return False


def _apply_to_regs(ins: DecodedInstruction,
                   regs: Dict[int, int]) -> Dict[int, int]:
    """Return regs updated by simulating `ins`.

    Whitelist of computable insns (MOV / MOVHI / MOVEA / ADDI / etc.)
    propagate exact values; any other writing insn invalidates the
    destination; r0 is always pinned to 0; BSU bitstring ops are
    treated as a maximal invalidation (they touch r26..r30 implicitly).

    Inputs whose source register is not in `regs` cause the destination
    to be invalidated — we never propagate from an unknown.
    """
    regs = dict(regs)
    regs[0] = 0  # r0 hardwired

    fmt = ins.fmt
    op = ins.opcode6

    # MOV rS, rD (Format I 0x00)
    if fmt is Format.I and op == 0x00:
        if ins.reg2 == 0:
            return regs
        if ins.reg1 in regs:
            regs[ins.reg2] = regs[ins.reg1]
        else:
            regs.pop(ins.reg2, None)
        return regs

    # MOV imm5, rD (Format II 0x10)
    if fmt is Format.II and op == 0x10:
        if ins.reg2 != 0:
            regs[ins.reg2] = ins.imm5_s & 0xFFFFFFFF
        return regs

    # ADD imm5, rD (Format II 0x11) — rD += sext(imm5)
    if fmt is Format.II and op == 0x11:
        if ins.reg2 != 0:
            if ins.reg2 in regs:
                regs[ins.reg2] = (regs[ins.reg2] + ins.imm5_s) & 0xFFFFFFFF
            else:
                regs.pop(ins.reg2, None)
        return regs

    # JAL writes r31 = pc + 4
    if fmt is Format.IV and op == 0x2B:
        regs[31] = (ins.pc + ins.size) & 0xFFFFFFFF
        return regs

    # Format V — three-operand imm16 arithmetic. r0 is hardwired so
    # the canonical "MOVHI #imm, r0, rD" loads the high half cleanly.
    if fmt is Format.V:
        if ins.reg2 == 0:
            return regs
        if ins.reg1 not in regs:
            regs.pop(ins.reg2, None)
            return regs
        a = regs[ins.reg1]
        if op == 0x28:    # MOVEA
            regs[ins.reg2] = (a + ins.imm16_s) & 0xFFFFFFFF
        elif op == 0x29:  # ADDI
            regs[ins.reg2] = (a + ins.imm16_s) & 0xFFFFFFFF
        elif op == 0x2C:  # ORI
            regs[ins.reg2] = (a | ins.imm16) & 0xFFFFFFFF
        elif op == 0x2D:  # ANDI
            regs[ins.reg2] = (a & ins.imm16) & 0xFFFFFFFF
        elif op == 0x2E:  # XORI
            regs[ins.reg2] = (a ^ ins.imm16) & 0xFFFFFFFF
        elif op == 0x2F:  # MOVHI
            regs[ins.reg2] = (a + (ins.imm16 << 16)) & 0xFFFFFFFF
        else:
            regs.pop(ins.reg2, None)
        return regs

    # BSU bitstring (Format VII 0x1F) — implicit r26..r30 + r28 deref.
    # Drop everything as the safe move; constant chains never survive
    # a bitstring anyway.
    if fmt is Format.VII and op == 0x1F:
        return {0: 0}

    if _is_no_write_insn(ins):
        return regs

    # Some other writing insn — invalidate the conventional destination.
    if ins.reg2 != 0:
        regs.pop(ins.reg2, None)
    return regs


def trace_reset_trampoline(rom: RomImage, *, max_steps: int = 8
                           ) -> Optional[int]:
    """Symbolically evaluate the thunk at the reset vector and return
    the cart's actual entry PC.

    Handles the common MOVHI/MOVEA/JMP pattern that every commercial
    VB cart uses, plus the MOV/ADD imm-five and JR/JAL fallbacks that
    show up in homebrew. Returns None if the trampoline uses an
    encoding that the constant tracker cannot evaluate; the caller
    can then decide whether to surface the failure or fall back to
    a JAL-only seed scan.
    """
    regs: Dict[int, int] = {0: 0}
    pc = RESET_VECTOR
    for _ in range(max_steps):
        ins = rom.decode_at_va(pc)
        if ins is None or ins.is_unknown:
            return None

        # Terminal: JMP rX
        if ins.fmt is Format.I and ins.opcode6 == 0x06:
            if ins.reg1 not in regs:
                return None
            return regs[ins.reg1] & 0xFFFFFFFF

        # Terminal: JR / JAL — both have a known target.
        if ins.fmt is Format.IV and ins.opcode6 in (0x2A, 0x2B):
            return ins.branch_target

        regs = _apply_to_regs(ins, regs)
        pc = (pc + ins.size) & 0xFFFFFFFF
    return None


@dataclass(frozen=True)
class FunctionRange:
    """One discovered function. `end_pc` is exclusive."""
    name: str
    start_pc: int
    end_pc: int


@dataclass(frozen=True)
class BasicBlock:
    block_id: int
    start_pc: int
    end_pc: int                # exclusive
    succ: Tuple[int, ...]      # successor block_ids
    pred: Tuple[int, ...]


@dataclass(frozen=True)
class ControlFlowGraph:
    function: FunctionRange
    blocks: Tuple[BasicBlock, ...]
    entry: int


@dataclass
class WalkResult:
    """Result of `cfg_walk_from_seeds`."""
    # PC -> decoded insn size (always 2 or 4). `visited.keys()` is the
    # set of every PC the walk actually decoded.
    visited: Dict[int, int] = field(default_factory=dict)
    # JAL targets discovered along the walk — these become function
    # seeds for `discover_functions`.
    call_targets: Set[int] = field(default_factory=set)
    # PCs at which JMP rX (X != 31) executed — opaque indirect
    # dispatch, typically a jump table. The walk stops on these
    # until later phases resolve them.
    indirect_jumps: Dict[int, int] = field(default_factory=dict)
    # PCs whose decoded insn was `is_unknown=True`. Recorded
    # explicitly so the L1 oracle test surfaces decoder gaps without
    # having to re-decode.
    unknown_pcs: Set[int] = field(default_factory=set)
    # Branch / jump targets that fell outside the cart mirror.
    # Recorded for diagnostics; the walk does not follow them.
    off_cart_pcs: Set[int] = field(default_factory=set)


def _is_terminator(ins: DecodedInstruction) -> bool:
    """Does this instruction end a control-flow path (no fall-through)?"""
    if ins.is_return:                                # JMP [r31]
        return True
    if ins.fmt is Format.IV and ins.opcode6 == 0x2A:  # JR (unconditional)
        return True
    if ins.fmt is Format.II and ins.opcode6 in (0x19, 0x1A, 0x18):
        # RETI, HALT, TRAP
        return True
    if (ins.fmt is Format.I and ins.opcode6 == 0x06
            and ins.reg1 != 31):
        # JMP rX with X != 31 — indirect dispatch.
        return True
    return False


def cfg_walk_from_seeds(rom: RomImage, seeds: Iterable[int]) -> WalkResult:
    """BFS over the V810 CFG starting at every seed PC.

    For each path the walker maintains a *path-local* register-constant
    map (see `_apply_to_regs`). The map is initialised to `{r0: 0}` at
    every seed and updated forward through linear instruction streams,
    invalidated by writes and dropped at branch joins. The walker uses
    this map only to resolve indirect `JMP rX` dispatches whose target
    register was set up by a static MOVHI/MOVEA chain (the canonical
    entry pattern, longcall thunks, tail-calls). Anything more
    ambitious belongs to a dedicated analysis pass, not the walker.

    Per-instruction control flow:
      * JAL (is_call):        record target as a function seed; queue
                              it with fresh regs; fall through with
                              regs[r31] = pc+4.
      * Bcond, conditional:   queue branch target with fresh regs;
                              fall through with current regs.
      * BR (always):          replace pc with target; no fall-through;
                              regs preserved into the taken path.
      * JR:                   replace pc with target; no fall-through;
                              regs preserved.
      * JMP rX, X != 31:      if regs[X] is known and in cart, treat
                              as a resolved jump (add to call_targets
                              too — almost always a function entry
                              when reached via static constants); else
                              record indirect and stop.
      * JMP [r31]/RETI/HALT/
        TRAP:                 stop the path.

    Unknown encodings: Format VII unknowns (FPP/BSU sub-op miss) keep
    walking past since size is reliable; reserved primaries stop the
    walk to avoid misaligning into data.
    """
    result = WalkResult()
    work: List[Tuple[int, Dict[int, int]]] = []
    for s in dict.fromkeys(seeds):
        work.append((s, {0: 0}))

    while work:
        pc, regs = work.pop()
        regs = dict(regs)
        while True:
            if pc in result.visited:
                break
            if not rom.can_read_insn(pc):
                result.off_cart_pcs.add(pc)
                break
            ins = rom.decode_at_va(pc)
            assert ins is not None
            result.visited[pc] = ins.size

            if ins.is_unknown:
                result.unknown_pcs.add(pc)
                if ins.fmt is Format.VII and ins.size in (2, 4):
                    pc = pc + ins.size
                    regs = {0: 0}  # unknown insn — no safe propagation
                    continue
                break

            if ins.is_call:
                tgt = ins.branch_target
                if tgt is not None and rom.va_to_offset(tgt) is not None:
                    result.call_targets.add(tgt)
                    if tgt not in result.visited:
                        work.append((tgt, {0: 0}))
                elif tgt is not None:
                    result.off_cart_pcs.add(tgt)
                regs = _apply_to_regs(ins, regs)
                pc = pc + ins.size
                continue

            if ins.is_branch:
                tgt = ins.branch_target
                always = (ins.fmt is Format.III and ins.cond == 0x5)
                if tgt is None:
                    pc = pc + ins.size
                    continue
                if rom.va_to_offset(tgt) is None:
                    result.off_cart_pcs.add(tgt)
                    if always:
                        break
                    pc = pc + ins.size
                    continue
                if always:
                    if tgt in result.visited:
                        break
                    pc = tgt
                    # regs preserved — single linear successor.
                    continue
                if tgt not in result.visited:
                    work.append((tgt, {0: 0}))
                pc = pc + ins.size
                continue

            if (ins.fmt is Format.I and ins.opcode6 == 0x06
                    and ins.reg1 != 31):
                # Indirect JMP rX. Try to resolve from current regs.
                if ins.reg1 in regs:
                    resolved = regs[ins.reg1] & 0xFFFFFFFF
                    if rom.va_to_offset(resolved) is not None:
                        # Treat the resolved target as a function entry —
                        # static dispatch through MOVHI/MOVEA is almost
                        # always the canonical entry-to-main thunk or a
                        # longcall.
                        result.call_targets.add(resolved)
                        if resolved not in result.visited:
                            work.append((resolved, {0: 0}))
                    else:
                        result.off_cart_pcs.add(resolved)
                else:
                    result.indirect_jumps[pc] = ins.reg1
                break

            if ins.fmt is Format.IV and ins.opcode6 == 0x2A:  # JR
                tgt = ins.branch_target
                if tgt is None:
                    break
                if rom.va_to_offset(tgt) is None:
                    result.off_cart_pcs.add(tgt)
                    break
                pc = tgt
                # regs preserved across direct JR.
                continue

            if _is_terminator(ins):
                break

            regs = _apply_to_regs(ins, regs)
            pc = pc + ins.size

    return result


def _walk_block_backwards(rom: RomImage, walk: WalkResult,
                          jump_pc: int, max_lookback: int
                          ) -> List[DecodedInstruction]:
    """Return the contiguous fall-through chain of insns ending at
    `jump_pc` within the same basic block. The chain stops as soon as
    a prior instruction is non-contiguous, is itself a branch/return,
    or after `max_lookback` steps.

    Used by the jump-table resolver to bound how far back we look for
    the LD/ANDI pattern.
    """
    pcs = sorted(walk.visited.keys())
    try:
        idx = pcs.index(jump_pc)
    except ValueError:
        return []
    chain_pcs: List[int] = [jump_pc]
    for j in range(idx - 1, max(-1, idx - max_lookback - 1), -1):
        prior_pc = pcs[j]
        prior = rom.decode_at_va(prior_pc)
        if prior is None or prior.is_unknown:
            break
        if prior_pc + prior.size != chain_pcs[0]:
            break
        # If the prior insn would itself stop control flow, the
        # current chain_pcs[0] cannot be a fall-through target.
        if prior.is_branch or prior.is_return:
            break
        if prior.fmt is Format.IV and prior.opcode6 == 0x2A:  # JR
            break
        if prior.fmt is Format.II and prior.opcode6 in (0x18, 0x19, 0x1A):
            break  # TRAP / RETI / HALT
        if prior.fmt is Format.I and prior.opcode6 == 0x06 and prior.reg1 != 31:
            break  # indirect JMP
        chain_pcs.insert(0, prior_pc)
    return [rom.decode_at_va(pc) for pc in chain_pcs]


def _try_resolve_jump_table(rom: RomImage, walk: WalkResult,
                            jump_pc: int, target_reg: int,
                            *, max_lookback: int = 24,
                            default_table_size: int = 16,
                            ) -> Set[int]:
    """Attempt to read a jump-table dispatched through `target_reg` at
    `jump_pc`. Pattern matched (across a single fall-through chain):

        ANDI #mask, rIdx, rIdx           (optional — bounds the table)
        ...
        MOVHI #hi, r0,    rBase
        ...                              (any constant ops)
        LD.W  disp(rBase), rTarget       (target_reg == rTarget)
        JMP   rTarget

    Returns the set of resolved target PCs, or an empty set if the
    pattern doesn't match cleanly. Targets that fall outside the
    cart mirror are dropped silently — they're either bogus reads
    past the end of the table or genuine pad bytes.
    """
    chain = _walk_block_backwards(rom, walk, jump_pc, max_lookback)
    if not chain:
        return set()

    # Scan the chain for: the most recent ANDI (table-bound mask),
    # the LD.W feeding the JMP, and any MOVHI that feeds the LD.W's
    # base register. Also accumulate constants forward so we can
    # resolve a fully-static base when present (longcall thunks).
    load_ins: Optional[DecodedInstruction] = None
    movhi_into_load_base: Optional[DecodedInstruction] = None
    andi_mask: Optional[int] = None
    regs: Dict[int, int] = {0: 0}
    for ins in chain[:-1]:
        if (ins.fmt is Format.V and ins.opcode6 == 0x2D
                and ins.imm16 != 0 and ins.imm16 < 0x1000):
            andi_mask = ins.imm16
        if (ins.fmt is Format.V and ins.opcode6 == 0x2F  # MOVHI
                and ins.reg2 != 0):
            # We won't know which MOVHI is the relevant one until we
            # see the LD.W; remember the latest MOVHI whose dest is
            # potentially the load's base.
            if load_ins is None:
                # Latest MOVHI seen so far; the LD.W may select it.
                pass
        if (ins.fmt is Format.VI and ins.opcode6 == 0x33
                and ins.reg2 == target_reg):
            load_ins = ins
            # Find the most recent prior MOVHI in `chain` whose dest
            # equals load_ins.reg1.
            for prev in reversed(chain[:chain.index(ins)]):
                if (prev.fmt is Format.V and prev.opcode6 == 0x2F
                        and prev.reg2 == load_ins.reg1):
                    movhi_into_load_base = prev
                    break
        regs = _apply_to_regs(ins, regs)

    if load_ins is None:
        return set()

    # Two ways to resolve the table base address.
    if load_ins.reg1 in regs:
        # Path A: the base register has a tracked constant. Typical
        # of fully-static dispatches (longcall thunks).
        table_base = (regs[load_ins.reg1] + load_ins.imm16_s) & 0xFFFFFFFF
    elif (movhi_into_load_base is not None
          and movhi_into_load_base.reg1 != 0):
        # Path B: the base register was constructed as
        #   rBase = MOVHI(imm16, rIdx)   ; rIdx == jump-table index
        # so its low half varies with the index, but the table's
        # effective start address — (imm16<<16) + LD.W_disp — is
        # still constant. This is the standard V810 jump-table emit.
        table_base = (((movhi_into_load_base.imm16 & 0xFFFF) << 16)
                      + load_ins.imm16_s) & 0xFFFFFFFF
    else:
        return set()

    table_size = (andi_mask + 1) if andi_mask is not None else default_table_size

    targets: Set[int] = set()
    for i in range(table_size):
        ent_va = (table_base + i * 4) & 0xFFFFFFFF
        off = rom.va_to_offset(ent_va)
        if off is None or off + 4 > rom.rom_size:
            break
        tgt = (rom.data[off]
               | (rom.data[off + 1] << 8)
               | (rom.data[off + 2] << 16)
               | (rom.data[off + 3] << 24))
        # Only accept entries that fall back into the cart mirror.
        if rom.va_to_offset(tgt) is not None:
            targets.add(tgt)
    return targets


def resolve_indirect_jumps(rom: RomImage, walk: WalkResult,
                           *, max_lookback: int = 24,
                           default_table_size: int = 16) -> Set[int]:
    """Try to resolve every unresolved indirect JMP into a set of
    concrete target PCs. Resolved jumps are removed from
    `walk.indirect_jumps` and their targets returned as a seed set
    for a follow-up CFG walk.

    Currently recognises the canonical V810 jump-table pattern
    (ANDI bounds, MOVHI/MOVEA constant base, LD.W table entry, JMP).
    Other indirect-dispatch shapes — function-pointer in WRAM,
    virtual-method-style vtables loaded from RAM, register-passed
    callbacks — remain unresolved; they need runtime observation
    or richer alias analysis than this static pass.
    """
    new_seeds: Set[int] = set()
    resolved: List[int] = []
    for jump_pc, reg in list(walk.indirect_jumps.items()):
        targets = _try_resolve_jump_table(
            rom, walk, jump_pc, reg,
            max_lookback=max_lookback,
            default_table_size=default_table_size,
        )
        if targets:
            new_seeds.update(targets)
            resolved.append(jump_pc)
    for pc in resolved:
        walk.indirect_jumps.pop(pc, None)
    return new_seeds


def _merge_walk_into(dst: WalkResult, src: WalkResult) -> None:
    dst.visited.update(src.visited)
    dst.call_targets.update(src.call_targets)
    dst.indirect_jumps.update(src.indirect_jumps)
    dst.unknown_pcs.update(src.unknown_pcs)
    dst.off_cart_pcs.update(src.off_cart_pcs)


def cfg_walk_with_table_resolution(rom: RomImage, seeds: Iterable[int],
                                   *, max_iters: int = 16) -> WalkResult:
    """High-level walker that iteratively resolves jump tables.

    Each iteration:
      1. Runs `cfg_walk_from_seeds` from the pending seeds.
      2. Calls `resolve_indirect_jumps` on the accumulated WalkResult.
      3. New seeds (resolved table targets) drive the next iteration.

    Resolved jump-table targets are merged into `call_targets` so the
    downstream codegen knows to give each one a recompiled-function
    entry. Without this, mid-function switch-case labels wouldn't
    appear in the dispatch table and vb_dispatch would fatal-abort
    when the table-driven JMP rX reaches them at runtime.

    Stops at `max_iters` (a sanity bound — typical carts converge in
    1-3 iterations) or when an iteration finds no new resolvable
    indirect jumps.
    """
    pending: Set[int] = set(seeds)
    aggregate = WalkResult()
    for _ in range(max_iters):
        if not pending:
            break
        step = cfg_walk_from_seeds(rom, sorted(pending))
        _merge_walk_into(aggregate, step)
        new_seeds = resolve_indirect_jumps(rom, aggregate)
        for tgt in new_seeds:
            if rom.va_to_offset(tgt) is not None:
                aggregate.call_targets.add(tgt)
        pending = {s for s in new_seeds if s not in aggregate.visited}
    return aggregate


def _collect_cross_function_targets(rom: RomImage, walk: WalkResult,
                                    entries: Iterable[int]) -> Set[int]:
    """Find every Bcond/JR target that lands in a *different* function's
    range and is not that function's head. These are the "rendezvous"
    PCs — typically a shared loop body or a fall-through landing pad
    where two functions interleave. Each one must become its own
    function entry so the dispatch table knows about it.

    Pure analysis pass — pure read of WalkResult; no mutation.
    """
    fn_starts = sorted(entries)
    if not fn_starts:
        return set()

    # Build a quick "PC → containing function" probe via bisect.
    def fn_containing(pc: int) -> Optional[int]:
        # Largest fn_start that is <= pc.
        lo, hi = 0, len(fn_starts)
        while lo < hi:
            mid = (lo + hi) // 2
            if fn_starts[mid] <= pc:
                lo = mid + 1
            else:
                hi = mid
        return fn_starts[lo - 1] if lo > 0 else None

    cross_targets: Set[int] = set()
    entry_set = set(fn_starts)

    for src_pc, size in walk.visited.items():
        ins = rom.decode_at_va(src_pc)
        if ins is None or ins.is_unknown:
            continue
        # We only care about intra-cart branch/jump targets.
        if ins.is_branch:
            tgt = ins.branch_target
        elif ins.fmt is Format.IV and ins.opcode6 == 0x2A:   # JR
            tgt = ins.branch_target
        else:
            continue
        if tgt is None or rom.va_to_offset(tgt) is None:
            continue
        src_fn = fn_containing(src_pc)
        tgt_fn = fn_containing(tgt)
        if src_fn is None or tgt_fn is None:
            continue
        # Same function — intra-function goto, no promotion needed.
        if src_fn == tgt_fn:
            continue
        # Target is the head of the other function — already in the
        # dispatch table.
        if tgt in entry_set:
            continue
        cross_targets.add(tgt)
    return cross_targets


def discover_functions(rom: RomImage) -> List[FunctionRange]:
    """Discover functions by CFG walk from the reset trampoline +
    every transitively-reached JAL target.

    Returns a list of `FunctionRange` sorted by `start_pc`. Each
    function spans from its entry PC to the highest visited byte
    inside its bucket (split at the next function entry). The
    bucketing is approximate — two physically-adjacent functions
    will merge if one falls through into the other via JR. That's
    accepted for L1 oracle parity (which compares per-instruction
    mnemonics, not function boundaries); P3's lifter will refine
    boundaries with dominator analysis if it ever matters.
    """
    # Seed from RESET_VECTOR rather than the resolved trampoline target.
    # The walker's per-path constant tracker handles the MOVHI/MOVEA/JMP
    # thunk and adds the resolved cart-entry to call_targets naturally
    # — and as a bonus the trampoline itself becomes a recompiled
    # function, so vb-runtime can boot by calling vb_dispatch(cpu,
    # RESET_VECTOR) without needing to know the cart-internal entry
    # PC at link time.
    if rom.va_to_offset(RESET_VECTOR) is None:
        return []
    # Seed the CFG walk from RESET_VECTOR and from every cart handler
    # vector. Vectors that pad to 0xFFFF cause the walker to record an
    # unknown encoding and stop — the walker is robust to that.
    seeds = [RESET_VECTOR] + [
        v for v in HANDLER_VECTORS if rom.va_to_offset(v) is not None
    ]
    walk = cfg_walk_with_table_resolution(rom, seeds)
    if not walk.visited:
        return []

    entries: Set[int] = set(seeds) | walk.call_targets
    entries = {e for e in entries if rom.va_to_offset(e) is not None}
    if not entries:
        return []

    # Cross-function-target promotion (P3 codegen requirement).
    # When function A's Bcond/JR lands in the middle of function B,
    # the dispatch table needs an entry for the landing PC. Promote
    # those PCs to function entries so each becomes addressable.
    # Iterates to fixpoint because each promotion can shrink an
    # existing function, exposing new cross-references that were
    # previously intra-function.
    for _ in range(8):
        cross = _collect_cross_function_targets(rom, walk, entries)
        if not cross:
            break
        entries |= cross

    fn_starts = sorted(entries)
    buckets: Dict[int, List[int]] = {e: [] for e in fn_starts}
    sorted_pcs = sorted(walk.visited.keys())

    idx = 0
    for pc in sorted_pcs:
        while idx + 1 < len(fn_starts) and fn_starts[idx + 1] <= pc:
            idx += 1
        anchor = fn_starts[idx] if pc >= fn_starts[0] else fn_starts[0]
        buckets[anchor].append(pc)

    fns: List[FunctionRange] = []
    for start in fn_starts:
        pcs = buckets[start]
        if not pcs:
            continue
        end = max(pc + walk.visited[pc] for pc in pcs)
        fns.append(FunctionRange(
            name=f"fn_{start:08X}",
            start_pc=start,
            end_pc=end,
        ))
    return fns


def build_cfg(fn: FunctionRange, rom: RomImage) -> ControlFlowGraph:
    """Per-function basic-block CFG.

    Block boundaries are determined by leaders:
      * the function entry,
      * any branch / jump target inside [start_pc, end_pc),
      * the instruction following any control-transfer.
    """
    leaders: Set[int] = {fn.start_pc}
    insns: Dict[int, DecodedInstruction] = {}
    visited: Set[int] = set()
    work: List[int] = [fn.start_pc]

    while work:
        cur = work.pop()
        while True:
            if cur in visited or not (fn.start_pc <= cur < fn.end_pc):
                break
            ins = rom.decode_at_va(cur)
            visited.add(cur)
            if ins is None or ins.is_unknown:
                break
            insns[cur] = ins
            nxt = cur + ins.size

            if ins.is_branch and ins.branch_target is not None:
                leaders.add(nxt)
                if fn.start_pc <= ins.branch_target < fn.end_pc:
                    leaders.add(ins.branch_target)
                    work.append(ins.branch_target)
                cur = nxt
                continue
            if ins.fmt is Format.IV and ins.opcode6 == 0x2A:    # JR
                if (ins.branch_target is not None
                        and fn.start_pc <= ins.branch_target < fn.end_pc):
                    leaders.add(ins.branch_target)
                    cur = ins.branch_target
                    continue
                break
            if ins.is_call:
                leaders.add(nxt)
                cur = nxt
                continue
            if _is_terminator(ins):
                break
            cur = nxt

    sorted_leaders = sorted(leaders)
    leader_to_id = {ldr: i for i, ldr in enumerate(sorted_leaders)}
    blocks: List[BasicBlock] = []

    for i, start in enumerate(sorted_leaders):
        end = sorted_leaders[i + 1] if i + 1 < len(sorted_leaders) else fn.end_pc
        last_pc = None
        cur = start
        while cur < end and cur in insns:
            last_pc = cur
            cur += insns[cur].size

        succ: List[int] = []
        if last_pc is not None:
            last = insns[last_pc]
            fall_through = last_pc + last.size
            if last.is_branch and last.branch_target in leader_to_id:
                succ.append(leader_to_id[last.branch_target])
                if fall_through in leader_to_id:
                    succ.append(leader_to_id[fall_through])
            elif (last.fmt is Format.IV and last.opcode6 == 0x2A
                    and last.branch_target in leader_to_id):
                succ.append(leader_to_id[last.branch_target])
            elif last.is_call:
                if fall_through in leader_to_id:
                    succ.append(leader_to_id[fall_through])
            elif not _is_terminator(last):
                if fall_through in leader_to_id:
                    succ.append(leader_to_id[fall_through])

        blocks.append(BasicBlock(
            block_id=i, start_pc=start, end_pc=end,
            succ=tuple(succ), pred=(),
        ))

    pred_by_id: Dict[int, List[int]] = {b.block_id: [] for b in blocks}
    for b in blocks:
        for s in b.succ:
            pred_by_id[s].append(b.block_id)
    blocks = [
        BasicBlock(b.block_id, b.start_pc, b.end_pc,
                   b.succ, tuple(pred_by_id[b.block_id]))
        for b in blocks
    ]

    return ControlFlowGraph(
        function=fn,
        blocks=tuple(blocks),
        entry=leader_to_id[fn.start_pc],
    )
