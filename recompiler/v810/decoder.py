"""V810 instruction decoder.

Decodes raw bytes into `DecodedInstruction` records. Reads operand
fields per format; does not interpret semantics.

Reserved opcodes, unknown FPP/BSU sub-opcodes, and bytes past the end
of the ROM yield explicit `Unknown` records — never a guess.
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterator, Optional

from .isa import (
    BCOND,
    BSU_SUBOP,
    FPP_SUBOP,
    Format,
    OpcodeSpec,
    is_bcond,
    lookup_primary,
)


@dataclass(frozen=True)
class DecodedInstruction:
    """One decoded V810 instruction."""

    pc: int              # virtual address of the first byte
    size: int            # 2 or 4
    raw: bytes           # exactly `size` bytes
    fmt: Format
    opcode6: int         # primary 6-bit opcode (bits 15:10 of first halfword)
    mnemonic: str        # canonical mnemonic (or sub-op mnemonic for VII / Bcond)
    reg1: int = 0        # bits 4:0 of first halfword (Format I, II, V, VI, VII)
    reg2: int = 0        # bits 9:5 of first halfword
    imm5: int = 0        # zero-extended 5-bit immediate (Format II)
    imm5_s: int = 0      # sign-extended 5-bit immediate (Format II)
    imm16: int = 0       # zero-extended 16-bit immediate (Format V/VI second halfword)
    imm16_s: int = 0     # sign-extended 16-bit immediate
    cond: int = 0        # Format III: bits 12:9
    disp9_s: int = 0     # Format III: sign-extended branch target offset (bytes)
    disp26_s: int = 0    # Format IV: sign-extended 26-bit displacement (bytes)
    branch_target: Optional[int] = None  # for Format III/IV; None otherwise
    subop: int = 0       # Format VII: sub-opcode field (FPP=2nd-hw 15:10, BSU=reg2)
    is_unknown: bool = False
    notes: str = ""

    @property
    def is_branch(self) -> bool:
        return self.fmt is Format.III and self.mnemonic != "NOP"

    @property
    def is_jump(self) -> bool:
        return self.fmt is Format.IV or (self.fmt is Format.I and self.opcode6 == 0x06)

    @property
    def is_call(self) -> bool:
        return self.fmt is Format.IV and self.opcode6 == 0x2B  # JAL

    @property
    def is_return(self) -> bool:
        # V810 has no dedicated RET; convention is JMP [r31].
        return self.fmt is Format.I and self.opcode6 == 0x06 and self.reg1 == 31

    @property
    def is_load(self) -> bool:
        return self.fmt is Format.VI and self.opcode6 in (0x30, 0x31, 0x33, 0x38, 0x39, 0x3B)

    @property
    def is_store(self) -> bool:
        return self.fmt is Format.VI and self.opcode6 in (0x34, 0x35, 0x37, 0x3C, 0x3D, 0x3F)


def _sext(value: int, bits: int) -> int:
    """Sign-extend `value` from `bits` bits to a signed Python int."""
    mask = (1 << bits) - 1
    sign = 1 << (bits - 1)
    value &= mask
    if value & sign:
        return value - (1 << bits)
    return value


def _u16le(buf: bytes, off: int) -> int:
    if off + 2 > len(buf):
        raise IndexError(f"truncated halfword at offset {off}")
    return buf[off] | (buf[off + 1] << 8)


def decode_at(buf: bytes, off: int, pc: int) -> DecodedInstruction:
    """Decode one instruction starting at byte offset `off`.

    `pc` is the virtual address the instruction would have when
    executed (used to compute branch targets).
    """
    if off >= len(buf):
        return DecodedInstruction(
            pc=pc, size=0, raw=b"", fmt=Format.I, opcode6=0,
            mnemonic="<eof>", is_unknown=True, notes="past end of buffer",
        )

    try:
        hw0 = _u16le(buf, off)
    except IndexError as e:
        return DecodedInstruction(
            pc=pc, size=1, raw=buf[off:off + 1], fmt=Format.I, opcode6=0,
            mnemonic="<truncated>", is_unknown=True, notes=str(e),
        )

    opcode6 = (hw0 >> 10) & 0x3F
    reg2 = (hw0 >> 5) & 0x1F
    reg1 = hw0 & 0x1F

    spec = lookup_primary(opcode6)

    # Format III: Bcond family
    if is_bcond(opcode6):
        cond = (hw0 >> 9) & 0xF
        disp9 = _sext(hw0 & 0x1FF, 9)
        # The LSB of disp9 is architecturally 0 (instructions are 2-byte aligned).
        mnemonic = BCOND.get(cond, f"Bcond<{cond:#x}>")
        target = (pc + disp9) & 0xFFFFFFFF
        return DecodedInstruction(
            pc=pc, size=2, raw=buf[off:off + 2], fmt=Format.III,
            opcode6=opcode6, mnemonic=mnemonic, cond=cond, disp9_s=disp9,
            branch_target=target if mnemonic != "NOP" else None,
        )

    # Reserved primary opcodes — fatal-LOUD intent, but decoder just
    # marks unknown; the runtime takes the final fatal action.
    if spec.opcode == -1:
        return DecodedInstruction(
            pc=pc, size=2, raw=buf[off:off + 2], fmt=Format.I,
            opcode6=opcode6, mnemonic=f"<reserved {opcode6:#04x}>",
            is_unknown=True, notes="NEC-reserved primary opcode",
        )

    fmt = spec.fmt
    size = spec.size

    # Read second halfword if 32-bit instruction.
    hw1 = 0
    if size == 4:
        try:
            hw1 = _u16le(buf, off + 2)
        except IndexError as e:
            return DecodedInstruction(
                pc=pc, size=min(2, len(buf) - off), raw=buf[off:off + 2],
                fmt=fmt, opcode6=opcode6, mnemonic=spec.mnemonic,
                reg1=reg1, reg2=reg2, is_unknown=True,
                notes=f"truncated 2nd halfword: {e}",
            )

    raw_bytes = buf[off:off + size]

    # Format I
    if fmt is Format.I:
        return DecodedInstruction(
            pc=pc, size=size, raw=raw_bytes, fmt=fmt, opcode6=opcode6,
            mnemonic=spec.mnemonic, reg1=reg1, reg2=reg2,
        )

    # Format II
    if fmt is Format.II:
        imm5 = reg1  # imm5 lives in bits 4:0
        return DecodedInstruction(
            pc=pc, size=size, raw=raw_bytes, fmt=fmt, opcode6=opcode6,
            mnemonic=spec.mnemonic, reg2=reg2,
            imm5=imm5, imm5_s=_sext(imm5, 5),
        )

    # Format IV (JR/JAL): disp26 = (hw0[9:0] << 16) | hw1, sign-ext 26.
    if fmt is Format.IV:
        disp26_raw = ((hw0 & 0x3FF) << 16) | hw1
        disp26_s = _sext(disp26_raw, 26)
        target = (pc + disp26_s) & 0xFFFFFFFF
        return DecodedInstruction(
            pc=pc, size=size, raw=raw_bytes, fmt=fmt, opcode6=opcode6,
            mnemonic=spec.mnemonic, disp26_s=disp26_s, branch_target=target,
        )

    # Format V: reg2, reg1, imm16
    if fmt is Format.V:
        return DecodedInstruction(
            pc=pc, size=size, raw=raw_bytes, fmt=fmt, opcode6=opcode6,
            mnemonic=spec.mnemonic, reg1=reg1, reg2=reg2,
            imm16=hw1, imm16_s=_sext(hw1, 16),
        )

    # Format VI: load/store/IN/OUT — disp16 (sign-extended) + base reg1 → reg2
    if fmt is Format.VI:
        return DecodedInstruction(
            pc=pc, size=size, raw=raw_bytes, fmt=fmt, opcode6=opcode6,
            mnemonic=spec.mnemonic, reg1=reg1, reg2=reg2,
            imm16=hw1, imm16_s=_sext(hw1, 16),
        )

    # Format VII
    if fmt is Format.VII:
        if opcode6 == 0x1F:
            # BSU: sub-op in reg2 field
            subop = reg2
            mnemonic = BSU_SUBOP.get(subop, f"BSU_UNK_{subop:#04x}")
            is_unk = subop not in BSU_SUBOP
            return DecodedInstruction(
                pc=pc, size=2, raw=buf[off:off + 2], fmt=fmt, opcode6=opcode6,
                mnemonic=mnemonic, reg1=reg1, reg2=reg2, subop=subop,
                is_unknown=is_unk,
                notes="bitstring; verify sub-op map against manual" if is_unk else "",
            )
        if opcode6 == 0x3E:
            # FPP: sub-op in bits 15:10 of second halfword
            subop = (hw1 >> 10) & 0x3F
            mnemonic = FPP_SUBOP.get(subop, f"FPP_UNK_{subop:#04x}")
            is_unk = subop not in FPP_SUBOP
            return DecodedInstruction(
                pc=pc, size=size, raw=raw_bytes, fmt=fmt, opcode6=opcode6,
                mnemonic=mnemonic, reg1=reg1, reg2=reg2,
                imm16=hw1, imm16_s=_sext(hw1, 16), subop=subop,
                is_unknown=is_unk,
                notes="FP/extended; verify sub-op against manual" if is_unk else "",
            )
        # CAXI moved to Format VI per the Sacred Tech Scroll; handled by
        # the generic Format VI branch above. No special case needed here.

    # Should be unreachable — every Format above is handled.
    return DecodedInstruction(
        pc=pc, size=size, raw=raw_bytes, fmt=fmt, opcode6=opcode6,
        mnemonic=spec.mnemonic, reg1=reg1, reg2=reg2,
        is_unknown=True, notes="decoder fall-through; this is a bug",
    )


def scan(buf: bytes, base_pc: int, *, start_offset: int = 0,
         end_offset: Optional[int] = None) -> Iterator[DecodedInstruction]:
    """Linearly decode `buf` starting at `start_offset`, yielding each
    instruction. Stops at `end_offset` (default: end of buffer).

    Linear scan; does NOT follow control flow. CFG construction lives
    in `analysis.py` (Phase 2).
    """
    if end_offset is None:
        end_offset = len(buf)
    off = start_offset
    while off < end_offset:
        ins = decode_at(buf, off, base_pc + off)
        yield ins
        # If the decoder couldn't determine a size (e.g. truncated EOF),
        # advance by 2 to keep scanning — the caller can still see the
        # is_unknown record.
        step = max(2, ins.size)
        off += step
