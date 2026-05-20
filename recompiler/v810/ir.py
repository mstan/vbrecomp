"""Intermediate representation for V810 → C lifting.

Shape borrowed from `snesrecomp/snesrecomp/recompiler/v2/ir.py`:
frozen dataclasses, explicit Read/Write nodes, no register-string
heuristics. P3 will populate the lifter; P1 ships the type shapes
so consumers can import them.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum, auto
from typing import Tuple


class Reg(Enum):
    """V810 register identifiers.

    GPRs are numbered 0..31. System registers are accessed via
    `LDSR`/`STSR` only and modelled as named entries below.
    """
    R0 = 0     # always zero
    R1 = 1
    R2 = 2
    R3 = 3
    R4 = 4
    R5 = 5
    R6 = 6
    R7 = 7
    R8 = 8
    R9 = 9
    R10 = 10
    R11 = 11
    R12 = 12
    R13 = 13
    R14 = 14
    R15 = 15
    R16 = 16
    R17 = 17
    R18 = 18
    R19 = 19
    R20 = 20
    R21 = 21
    R22 = 22
    R23 = 23
    R24 = 24
    R25 = 25
    R26 = 26
    R27 = 27
    R28 = 28
    R29 = 29  # sp by convention
    R30 = 30
    R31 = 31  # lp (link register)
    PC = 100

    # System registers (LDSR/STSR with imm5 index)
    EIPC   = 200
    EIPSW  = 201
    FEPC   = 202
    FEPSW  = 203
    ECR    = 204
    PSW    = 205
    PIR    = 206
    TKCW   = 207
    CHCW   = 224
    ADTRE  = 225


class AluKind(Enum):
    ADD = auto()
    SUB = auto()
    CMP = auto()
    MUL = auto()
    MULU = auto()
    DIV = auto()
    DIVU = auto()
    OR = auto()
    AND = auto()
    XOR = auto()
    NOT = auto()
    SHL = auto()
    SHR = auto()
    SAR = auto()
    MOV = auto()
    MOVHI = auto()
    MOVEA = auto()
    SETF = auto()


class CondKind(Enum):
    BV  = 0x0
    BL  = 0x1
    BE  = 0x2
    BNH = 0x3
    BN  = 0x4
    BR  = 0x5
    BLT = 0x6
    BLE = 0x7
    BNV = 0x8
    BNL = 0x9
    BNE = 0xA
    BH  = 0xB
    BP  = 0xC
    NOP = 0xD
    BGE = 0xE
    BGT = 0xF


@dataclass(frozen=True)
class Value:
    """Opaque IR value handle.

    Identity is `(block_id, op_index)`. P2 wiring fills these in;
    P1 ships the dataclass shape.
    """
    block_id: int
    op_index: int


@dataclass(frozen=True)
class IROp:
    """Base for all IR ops. Subclasses are frozen dataclasses."""
    pc: int


@dataclass(frozen=True)
class ReadReg(IROp):
    reg: Reg


@dataclass(frozen=True)
class WriteReg(IROp):
    reg: Reg
    src: Value


@dataclass(frozen=True)
class ConstI(IROp):
    value: int
    bits: int = 32   # 32 by default; 5 for imm5, 16 for imm16


@dataclass(frozen=True)
class AluOp(IROp):
    kind: AluKind
    lhs: Value
    rhs: Value
    # Whether PSW flags Z/S/CY/OV update (some MOV variants do not).
    flag_update: bool = True


@dataclass(frozen=True)
class Read(IROp):
    addr: Value
    width: int   # 8 / 16 / 32 bits


@dataclass(frozen=True)
class Write(IROp):
    addr: Value
    val: Value
    width: int


@dataclass(frozen=True)
class BranchCond(IROp):
    cond: CondKind
    target: int    # absolute PC


@dataclass(frozen=True)
class Jmp(IROp):
    target_reg: Reg


@dataclass(frozen=True)
class Jr(IROp):
    target: int


@dataclass(frozen=True)
class Jal(IROp):
    target: int
    # link = pc + 4, written to r31 by the lifter as a separate WriteReg.


@dataclass(frozen=True)
class Trap(IROp):
    vector: int


@dataclass(frozen=True)
class Reti(IROp):
    pass


@dataclass(frozen=True)
class Halt(IROp):
    pass


@dataclass(frozen=True)
class BitString(IROp):
    """Bitstring instruction (SCHnBSU/D, MOVBSU, ORBSU, etc.).

    All bitstring ops consume r26..r30 implicitly:
      r26: bit length
      r27: bit offset for dest
      r28: dest word address
      r29: bit offset for src
      r30: src word address
    The IR node records the sub-op only; the lifter expands the
    register reads / memory traffic explicitly.
    """
    subop: int
    mnemonic: str


@dataclass(frozen=True)
class Float(IROp):
    """V810 FP / extension op (ADDF.S, SUBF.S, CVT.WS, REV, XB, XH, ...).

    Sub-op identification by mnemonic; concrete operand reads/writes
    expanded by the lifter.
    """
    subop: int
    mnemonic: str


@dataclass(frozen=True)
class LdSr(IROp):
    sysreg: Reg
    src: Value


@dataclass(frozen=True)
class StSr(IROp):
    sysreg: Reg


# Placeholder for future passes.
@dataclass(frozen=True)
class Block:
    block_id: int
    start_pc: int
    end_pc: int
    ops: Tuple[IROp, ...]
