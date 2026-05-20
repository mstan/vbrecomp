"""Parse Beetle VB's `v810_op_table_msvc.inc` into a canonical
opcode-index -> mnemonic map.

This file is the L1 oracle's static source. Instead of bringing up a
TCP service against `vb-beetle.exe`, the L1 test reads Beetle's own
dispatch table — `case <idx>: goto op_<NAME>;` — and uses the
extracted op_NAME for each 7-bit dispatch key as the oracle mnemonic.

The dispatch key is `(hw0 >> 9) & 0x7F`. For non-Bcond primary
opcodes that's `(primary << 1) | bit9_of_reg2_field`; for Bcond
(primaries 0x20-0x27) that's `0x40 + cond4`. Beetle's table doubles
non-Bcond entries (two cases per primary) and gives each Bcond cond
its own slot — exactly the shape we need.

Naming aliases — translated here, NOT silently accepted in the test:

  - Beetle `op_EI`  ↔ ours `CLI`   (NEC manual canonical name)
  - Beetle `op_DI`  ↔ ours `SEI`   (NEC manual canonical name)
  - Beetle `op_MOV_I` ↔ ours `MOV` (Format II MOV imm5 vs Format I MOV)
  - Beetle `op_ADD_I` ↔ ours `ADD`
  - Beetle `op_CMP_I` ↔ ours `CMP`
  - Beetle `op_SHL_I` ↔ ours `SHL`
  - Beetle `op_SHR_I` ↔ ours `SHR`
  - Beetle `op_SAR_I` ↔ ours `SAR`
  - Beetle `op_LD_B/H/W` ↔ ours `LD.B/H/W`   (mednafen uses underscore)
  - Beetle `op_ST_B/H/W` ↔ ours `ST.B/H/W`
  - Beetle `op_IN_B/H/W` ↔ ours `IN.B/H/W`
  - Beetle `op_OUT_B/H/W` ↔ ours `OUT.B/H/W`

`op_BSTR` and `op_FPP` are stub dispatchers — they fan out to BSU /
FPP sub-opcode handlers. The test consults `BSU_SUBOP` / `FPP_SUBOP`
from `recompiler.v810.isa` for the resolved sub-op mnemonic.

`op_INVALID` and `op_INT_HANDLER` are non-mnemonic — Beetle traps to
its invalid-op handler. We map both to None, and the test verifies
our decoder marks the instruction `is_unknown`.
"""
from __future__ import annotations

import re
from pathlib import Path
from typing import Dict, Optional


BEETLE_TO_OURS: Dict[str, Optional[str]] = {
    "op_MOV": "MOV",
    "op_ADD": "ADD",
    "op_SUB": "SUB",
    "op_CMP": "CMP",
    "op_SHL": "SHL",
    "op_SHR": "SHR",
    "op_JMP": "JMP",
    "op_SAR": "SAR",
    "op_MUL": "MUL",
    "op_DIV": "DIV",
    "op_MULU": "MULU",
    "op_DIVU": "DIVU",
    "op_OR": "OR",
    "op_AND": "AND",
    "op_XOR": "XOR",
    "op_NOT": "NOT",

    "op_MOV_I": "MOV",
    "op_ADD_I": "ADD",
    "op_SETF":  "SETF",
    "op_CMP_I": "CMP",
    "op_SHL_I": "SHL",
    "op_SHR_I": "SHR",
    "op_SAR_I": "SAR",
    "op_EI":    "CLI",
    "op_DI":    "SEI",
    "op_TRAP":  "TRAP",
    "op_RETI":  "RETI",
    "op_HALT":  "HALT",
    "op_LDSR":  "LDSR",
    "op_STSR":  "STSR",

    "op_BV": "BV", "op_BL": "BL", "op_BE": "BE", "op_BNH": "BNH",
    "op_BN": "BN", "op_BR": "BR", "op_BLT": "BLT", "op_BLE": "BLE",
    "op_BNV": "BNV", "op_BNL": "BNL", "op_BNE": "BNE", "op_BH": "BH",
    "op_BP": "BP", "op_NOP": "NOP", "op_BGE": "BGE", "op_BGT": "BGT",

    "op_MOVEA": "MOVEA",
    "op_ADDI":  "ADDI",
    "op_JR":    "JR",
    "op_JAL":   "JAL",
    "op_ORI":   "ORI",
    "op_ANDI":  "ANDI",
    "op_XORI":  "XORI",
    "op_MOVHI": "MOVHI",

    "op_LD_B": "LD.B", "op_LD_H": "LD.H", "op_LD_W": "LD.W",
    "op_ST_B": "ST.B", "op_ST_H": "ST.H", "op_ST_W": "ST.W",
    "op_IN_B": "IN.B", "op_IN_H": "IN.H", "op_IN_W": "IN.W",
    "op_OUT_B": "OUT.B", "op_OUT_H": "OUT.H", "op_OUT_W": "OUT.W",
    "op_CAXI": "CAXI",

    "op_BSTR": None,            # resolved via reg2 sub-op
    "op_FPP":  None,            # resolved via 2nd-hw sub-op
    "op_INVALID": None,         # decoder must flag is_unknown
    "op_INT_HANDLER": None,     # never appears in cart code
}


# Parse `case <idx>: ... goto op_<NAME>;` blocks. Multiple consecutive
# `case` labels share one goto target, so we accept up to four
# stacked `case` labels in front of the goto — that's the most Beetle
# ever uses.
_CASE_PATTERN = re.compile(
    r'case\s+(\d+):\s*'
    r'(?:case\s+(\d+):\s*)?'
    r'(?:case\s+(\d+):\s*)?'
    r'(?:case\s+(\d+):\s*)?'
    r'goto\s+(op_\w+)\s*;',
    re.MULTILINE,
)


def parse_op_table(path: Path | str) -> Dict[int, str]:
    """Return a `{dispatch_index: op_NAME}` map parsed from a Beetle
    `v810_op_table_msvc.inc` (or compatible). Indices that fall in
    the `default:` arm — i.e. anything past the explicit cases —
    are *not* present in the result; callers should treat absent
    indices as `op_INVALID`.
    """
    text = Path(path).read_text(encoding="utf-8")
    table: Dict[int, str] = {}
    for match in _CASE_PATTERN.finditer(text):
        op = match.group(5)
        for grp in match.groups()[:4]:
            if grp is not None:
                table[int(grp)] = op
    return table


def beetle_op_for_hw0(table: Dict[int, str], hw0: int) -> str:
    """Look up Beetle's op_NAME for an instruction's first halfword.

    Indices outside the explicit `case` arms fall through Beetle's
    `default: goto op_INVALID;`, so we return `"op_INVALID"` for any
    miss to match that behaviour exactly.
    """
    idx = (hw0 >> 9) & 0x7F
    return table.get(idx, "op_INVALID")


def expected_mnemonic_for_op(op_name: str) -> Optional[str]:
    """Translate a Beetle op_NAME to the mnemonic our decoder produces.

    None means "Beetle would treat this as an invalid encoding" (or
    the op is a sub-op dispatcher — caller must resolve further).
    """
    return BEETLE_TO_OURS.get(op_name)
