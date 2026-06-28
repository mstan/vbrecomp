"""V810 per-instruction base cycle costs — the single source of truth
for the Axis-2 cycle model.

Goal (VB_ACCURACY_BURNDOWN.md Axis 2): replace the runtime's coarse
`cyc_delta = bbs_run * 3` estimate with a real per-instruction cycle
sum, so guest-visible device time (timer / VIP / VSU) advances at the
true rate. This kills the residual audio *tempo drift* and lets the
`cyc_watch` ring converge (Delta -> 0) against the oracle's guest-cycle
counter.

SOURCING (CLAUDE.md Rule 2 / Rule 12 — cite, never guess)
---------------------------------------------------------
Every value below is the oracle's own `ADDCLOCK(n)` constant from
Mednafen-VB `beetle-vb/mednafen/hw_cpu/v810/v810_oploop.inc` (line
numbers cited per op). The oracle is the de-facto VB reference and the
deterministic Delta-convergence target for this axis. Where the V810
Architecture Manual numbers ARE documented in-tree
(`docs/HARDWARE_NOTES.md:208-209`) they AGREE with the oracle:
ALU = 1, MUL = 13, DIV = 38, branch-taken = 3. The oracle additionally
splits DIVU = 36 (manual gives a single "divide = 38").

These are BASE costs. The oracle also models, and this first cut does
NOT yet model (tracked refinement — measure the residual with
`cyc_watch` before adding):
  * load/store pipeline pairing — `lastop` +1 same-class / +2 otherwise
    (v810_oploop.inc:565-692). Effective standalone load ~3, not 1.
  * 16-bit-bus split penalties on LD.W/ST.W/IN.W (v810_oploop.inc:632-689).
  * FP ops are charged a flat 1 by the oracle (`:892`) though real FP is
    multi-cycle — the oracle does not model FP timing, so neither do we.
The manual vs oracle load discrepancy (manual "load = 4 cache miss" vs
oracle base 1 + pairing) is deliberately resolved toward the oracle here
because the oracle is the runtime Delta reference; revisit if `cyc_watch`
shows loads dominate the residual.

Each V810 6-bit primary opcode maps to exactly one op (Format I 0x00-0x0F,
II 0x10-0x1F, Bcond 0x20-0x27, IV/V 0x28-0x2F, VI 0x30-0x3F, plus BSU at
0x1F and FPP at 0x3E), so a flat opcode6 -> cost table is unambiguous.
"""
from __future__ import annotations

from .decoder import DecodedInstruction
from .isa import Format

# Extra cycles a conditional branch costs when TAKEN, on top of the
# not-taken base of 1 (oracle: ADDCLOCK(3) taken at v810_oploop.inc:420,
# ADDCLOCK(1) not-taken at :430). Charged inside the taken path by the
# emitter; the base 1 is charged inline for every Bcond.
BCOND_TAKEN_EXTRA = 2

# opcode6 -> base cycle cost. Bcond (0x20-0x27) carry the NOT-TAKEN base.
_BASE_CYCLES: dict[int, int] = {
    # ---- Format I (reg/reg, 2 bytes) — oploop.inc:141-248,812-859 ----
    0x00: 1,   # MOV   :141
    0x01: 1,   # ADD   :147
    0x02: 1,   # SUB   :159
    0x03: 1,   # CMP   :171
    0x04: 1,   # SHL   :181
    0x05: 1,   # SHR   :192
    0x06: 3,   # JMP   :205  (terminator; charged inline before transfer)
    0x07: 1,   # SAR   :215
    0x08: 13,  # MUL   :812
    0x09: 38,  # DIV   :859
    0x0A: 13,  # MULU  :824
    0x0B: 36,  # DIVU  :836
    0x0C: 1,   # OR    :227
    0x0D: 1,   # AND   :234
    0x0E: 1,   # XOR   :241
    0x0F: 1,   # NOT   :248
    # ---- Format II (imm5/reg + system, 2 bytes) — :255-409,789,796,927 ----
    0x10: 1,   # MOV imm5  :255
    0x11: 1,   # ADD imm5  :260
    0x12: 1,   # SETF      :272
    0x13: 1,   # CMP imm5  :330
    0x14: 1,   # SHL imm5  :339
    0x15: 1,   # SHR imm5  :348
    0x16: 1,   # CLI       :384
    0x17: 1,   # SAR imm5  :358
    0x18: 15,  # TRAP      :927
    0x19: 10,  # RETI      :796
    0x1A: 1,   # HALT      (oracle does not ADDCLOCK HALT separately; the
               #            instr itself is ~1, then the idle path drives
               #            device time — see main.cpp HALT branch)
    0x1C: 1,   # LDSR      :368  (oracle marks "/* ? */")
    0x1D: 1,   # STSR      :374  (oracle marks "/* ? */")
    0x1E: 1,   # SEI       :403
    0x1F: 1,   # BSU entry :901  (per-iteration cost not modeled; the
               #            emitter aborts on BSU for Mario's Tennis)
    # ---- Format III Bcond (0x20-0x27): not-taken base = 1 (:430) ----
    0x20: 1, 0x21: 1, 0x22: 1, 0x23: 1,
    0x24: 1, 0x25: 1, 0x26: 1, 0x27: 1,
    # ---- Format IV / V (4 bytes) — :496-554 ----
    0x28: 1,   # MOVEA :517
    0x29: 1,   # ADDI  :522
    0x2A: 3,   # JR    :496  (terminator; charged inline)
    0x2B: 3,   # JAL   :506  (terminator; charged inline)
    0x2C: 1,   # ORI   :533
    0x2D: 1,   # ANDI  :540
    0x2E: 1,   # XORI  :547
    0x2F: 1,   # MOVHI :554
    # ---- Format VI (load/store/IN/OUT, 4 bytes) — :560-783,936 ----
    0x30: 1,   # LD.B  :560  (base; pairing deferred)
    0x31: 1,   # LD.H  :583
    0x33: 1,   # LD.W  :604
    0x34: 1,   # ST.B  :645
    0x35: 1,   # ST.H  :657
    0x37: 1,   # ST.W  :670
    0x38: 3,   # IN.B  :698
    0x39: 3,   # IN.H  :708
    0x3A: 26,  # CAXI  :936
    0x3B: 3,   # IN.W  :719
    0x3C: 1,   # OUT.B :735
    0x3D: 1,   # OUT.H :748
    0x3E: 1,   # FPP   :892  (oracle flat 1; FP timing not modeled)
    0x3F: 1,   # OUT.W :761
}


def instr_base_cycles(ins: DecodedInstruction) -> int:
    """V810 base cycle cost of one decoded instruction (NOT-TAKEN base
    for conditional branches; the taken extra is BCOND_TAKEN_EXTRA).

    Sourced from the oracle's ADDCLOCK constants (see module docstring).
    Reserved/unknown opcodes (0x1B, 0x32, 0x36) are not in the table —
    the decoder elevates those to a fatal abort, so the executed path
    never charges them; we return 1 so codegen does not crash on a
    misdecoded data byte, never a silent guess at a real cost.
    """
    cost = _BASE_CYCLES.get(ins.opcode6)
    if cost is None:
        return 1
    return cost
