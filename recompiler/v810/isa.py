"""V810 instruction set — opcode → format + mnemonic table.

The 6-bit primary opcode lives in bits 15:10 of the first halfword.
Format determines whether the instruction is 16 or 32 bits and how
operand fields are laid out.

Source: NEC V810 Architecture Manual + Virtual Boy Programmer's
Manual. Cross-check against the manual when in doubt; this file is
the project's single source of truth for what the decoder recognises,
and `docs/INSTRUCTION_STATUS.md` mirrors it.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto


class Format(Enum):
    """V810 instruction formats."""
    I = auto()    # 2 bytes: opcode | reg2 | reg1
    II = auto()   # 2 bytes: opcode | reg2 | imm5
    III = auto()  # 2 bytes: 100 | cond4 | disp9      (Bcond)
    IV = auto()   # 4 bytes: opcode | disp26          (JR / JAL)
    V = auto()    # 4 bytes: opcode | reg2 | reg1 | imm16
    VI = auto()   # 4 bytes: opcode | reg2 | reg1 | disp16  (load/store, IN/OUT)
    VII = auto()  # 2 or 4 bytes: extension (FPP / bitstring / CAXI / etc.)


@dataclass(frozen=True)
class OpcodeSpec:
    """Static info about one primary 6-bit opcode."""
    opcode: int          # 0x00 – 0x3F
    mnemonic: str        # canonical V810 mnemonic
    fmt: Format
    size: int            # 2 or 4 (overridden for Format III which is always 2)
    notes: str = ""


# Sentinel for "this 6-bit slot is reserved by NEC — fatal on decode".
_RESERVED = OpcodeSpec(opcode=-1, mnemonic="<reserved>", fmt=Format.I, size=2)


# Primary 6-bit opcode table.
#
# Format-III conditional branches occupy 0x20..0x27 (top 3 bits = 100).
# The decoder treats those 8 entries as a family and dispatches on the
# 4-bit cond in bits 12:9.
PRIMARY: dict[int, OpcodeSpec] = {
    # ---- Format I: register / register (2 bytes) ----
    0x00: OpcodeSpec(0x00, "MOV",  Format.I, 2),
    0x01: OpcodeSpec(0x01, "ADD",  Format.I, 2),
    0x02: OpcodeSpec(0x02, "SUB",  Format.I, 2),
    0x03: OpcodeSpec(0x03, "CMP",  Format.I, 2),
    0x04: OpcodeSpec(0x04, "SHL",  Format.I, 2),
    0x05: OpcodeSpec(0x05, "SHR",  Format.I, 2),
    0x06: OpcodeSpec(0x06, "JMP",  Format.I, 2, notes="uses reg1 only (jump target)"),
    0x07: OpcodeSpec(0x07, "SAR",  Format.I, 2),
    0x08: OpcodeSpec(0x08, "MUL",  Format.I, 2),
    0x09: OpcodeSpec(0x09, "DIV",  Format.I, 2),
    0x0A: OpcodeSpec(0x0A, "MULU", Format.I, 2),
    0x0B: OpcodeSpec(0x0B, "DIVU", Format.I, 2),
    0x0C: OpcodeSpec(0x0C, "OR",   Format.I, 2),
    0x0D: OpcodeSpec(0x0D, "AND",  Format.I, 2),
    0x0E: OpcodeSpec(0x0E, "XOR",  Format.I, 2),
    0x0F: OpcodeSpec(0x0F, "NOT",  Format.I, 2),

    # ---- Format II: imm5 + reg (2 bytes) ----
    0x10: OpcodeSpec(0x10, "MOV",  Format.II, 2, notes="mov imm5,reg2"),
    0x11: OpcodeSpec(0x11, "ADD",  Format.II, 2, notes="add imm5,reg2"),
    0x12: OpcodeSpec(0x12, "SETF", Format.II, 2, notes="set reg2 = (cond imm5 met) ? 1 : 0"),
    0x13: OpcodeSpec(0x13, "CMP",  Format.II, 2, notes="cmp imm5,reg2"),
    0x14: OpcodeSpec(0x14, "SHL",  Format.II, 2, notes="shl imm5,reg2"),
    0x15: OpcodeSpec(0x15, "SHR",  Format.II, 2, notes="shr imm5,reg2"),
    0x16: OpcodeSpec(0x16, "CLI",  Format.II, 2, notes="ID := 0 (enable interrupts)"),
    0x17: OpcodeSpec(0x17, "SAR",  Format.II, 2, notes="sar imm5,reg2"),
    0x18: OpcodeSpec(0x18, "TRAP", Format.II, 2, notes="vector in imm5"),
    0x19: OpcodeSpec(0x19, "RETI", Format.II, 2),
    0x1A: OpcodeSpec(0x1A, "HALT", Format.II, 2),
    0x1B: _RESERVED,
    0x1C: OpcodeSpec(0x1C, "LDSR", Format.II, 2, notes="ldsr reg2,sysreg(imm5)"),
    0x1D: OpcodeSpec(0x1D, "STSR", Format.II, 2, notes="stsr sysreg(imm5),reg2"),
    0x1E: OpcodeSpec(0x1E, "SEI",  Format.II, 2, notes="ID := 1 (disable interrupts)"),
    0x1F: OpcodeSpec(0x1F, "BSU",  Format.VII, 2,
                     notes="bitstring op; sub-op in reg2 field (bits 9:5) per NEC spec"),

    # ---- Format III: Bcond (occupies 0x20..0x27 — opcode high bits 100) ----
    # Decoder handles these as a family; entries here are documentation.
    0x20: OpcodeSpec(0x20, "Bcond", Format.III, 2),
    0x21: OpcodeSpec(0x21, "Bcond", Format.III, 2),
    0x22: OpcodeSpec(0x22, "Bcond", Format.III, 2),
    0x23: OpcodeSpec(0x23, "Bcond", Format.III, 2),
    0x24: OpcodeSpec(0x24, "Bcond", Format.III, 2),
    0x25: OpcodeSpec(0x25, "Bcond", Format.III, 2),
    0x26: OpcodeSpec(0x26, "Bcond", Format.III, 2),
    0x27: OpcodeSpec(0x27, "Bcond", Format.III, 2),

    # ---- Format V (mostly) and IV ----
    0x28: OpcodeSpec(0x28, "MOVEA", Format.V,  4),
    0x29: OpcodeSpec(0x29, "ADDI",  Format.V,  4),
    0x2A: OpcodeSpec(0x2A, "JR",    Format.IV, 4),
    0x2B: OpcodeSpec(0x2B, "JAL",   Format.IV, 4),
    0x2C: OpcodeSpec(0x2C, "ORI",   Format.V,  4),
    0x2D: OpcodeSpec(0x2D, "ANDI",  Format.V,  4),
    0x2E: OpcodeSpec(0x2E, "XORI",  Format.V,  4),
    0x2F: OpcodeSpec(0x2F, "MOVHI", Format.V,  4),

    # ---- Format VI: load / store / IN / OUT (4 bytes) ----
    0x30: OpcodeSpec(0x30, "LD.B",  Format.VI, 4),
    0x31: OpcodeSpec(0x31, "LD.H",  Format.VI, 4),
    0x32: _RESERVED,
    0x33: OpcodeSpec(0x33, "LD.W",  Format.VI, 4),
    0x34: OpcodeSpec(0x34, "ST.B",  Format.VI, 4),
    0x35: OpcodeSpec(0x35, "ST.H",  Format.VI, 4),
    0x36: _RESERVED,
    0x37: OpcodeSpec(0x37, "ST.W",  Format.VI, 4),
    0x38: OpcodeSpec(0x38, "IN.B",  Format.VI, 4),
    0x39: OpcodeSpec(0x39, "IN.H",  Format.VI, 4),
    0x3A: OpcodeSpec(0x3A, "CAXI",  Format.VI, 4, notes="compare-and-exchange interlocked (per Sacred Tech Scroll: Format VI)"),
    0x3B: OpcodeSpec(0x3B, "IN.W",  Format.VI, 4),
    0x3C: OpcodeSpec(0x3C, "OUT.B", Format.VI, 4),
    0x3D: OpcodeSpec(0x3D, "OUT.H", Format.VI, 4),
    0x3E: OpcodeSpec(0x3E, "FPP",   Format.VII, 4,
                     notes="FP / extension; sub-op in bits 15:10 of 2nd halfword"),
    0x3F: OpcodeSpec(0x3F, "OUT.W", Format.VI, 4),
}


# 4-bit Bcond condition codes (bits 12:9 of the first halfword).
BCOND: dict[int, str] = {
    0x0: "BV",      # overflow
    0x1: "BL",      # carry / lower (BC)
    0x2: "BE",      # equal / zero (BZ)
    0x3: "BNH",     # not higher (C | Z)
    0x4: "BN",      # negative
    0x5: "BR",      # always (unconditional)
    0x6: "BLT",     # signed less than (S ^ OV)
    0x7: "BLE",     # signed less or equal ((S ^ OV) | Z)
    0x8: "BNV",     # not overflow
    0x9: "BNL",     # not carry (BNC)
    0xA: "BNE",     # not equal (BNZ)
    0xB: "BH",      # higher (~C & ~Z)
    0xC: "BP",      # positive
    0xD: "NOP",     # Bcond with cond=0xD encodes NOP (never branch)
    0xE: "BGE",     # signed greater or equal (~(S ^ OV))
    0xF: "BGT",     # signed greater than (~((S ^ OV) | Z))
}


# Sub-opcodes for primary 0x3E (FPP / extended) — second halfword bits 15:10.
# This table is partial; unknown sub-ops decode as `FPP_UNK_<n>` and surface
# in INSTRUCTION_STATUS.md as decoded:no.
FPP_SUBOP: dict[int, str] = {
    0x00: "CMPF.S",
    0x02: "CVT.WS",
    0x03: "CVT.SW",
    0x04: "ADDF.S",
    0x05: "SUBF.S",
    0x06: "MULF.S",
    0x07: "DIVF.S",
    0x08: "XB",
    0x09: "XH",
    0x0A: "REV",
    0x0B: "TRNC.SW",
    0x0C: "MPYHW",
}


# Sub-opcodes for primary 0x1F (BSU bitstring) — in the reg2 field
# (bits 9:5) of the first halfword. Cross-check the manual before
# trusting any specific encoding here; the table below is the most
# commonly-cited assignment but several community references disagree
# on individual indices.
BSU_SUBOP: dict[int, str] = {
    0x00: "SCH0BSU",
    0x01: "SCH0BSD",
    0x02: "SCH1BSU",
    0x03: "SCH1BSD",
    0x08: "ORBSU",
    0x09: "ANDBSU",
    0x0A: "XORBSU",
    0x0B: "MOVBSU",
    0x0C: "ORNBSU",
    0x0D: "ANDNBSU",
    0x0E: "XORNBSU",
    0x0F: "NOTBSU",
}


def lookup_primary(opcode6: int) -> OpcodeSpec:
    """Return the OpcodeSpec for a 6-bit primary opcode.

    Raises ValueError if the opcode is outside 0..63.
    Returns the _RESERVED sentinel for slots NEC documents as
    reserved — the decoder elevates those to UnknownOpcode results
    rather than guessing.
    """
    if not 0 <= opcode6 < 64:
        raise ValueError(f"primary opcode out of range: {opcode6:#x}")
    return PRIMARY.get(opcode6, _RESERVED)


def is_bcond(opcode6: int) -> bool:
    """Is this opcode one of the eight Bcond entries (0x20..0x27)?"""
    return 0x20 <= opcode6 <= 0x27
