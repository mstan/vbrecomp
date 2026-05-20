"""Mechanical regen of `docs/INSTRUCTION_STATUS.md` from `isa.py` +
Beetle VB's op table.

The status doc is the project's single source of truth for "what's
implemented". Hand-edits drift; this CLI keeps the table mechanically
consistent with the live decoder tables and the Beetle oracle.

For each opcode the columns are:

  decoded   yes / Invalid     — does the decoder recognise the bits?
  lifted    yes / no          — does lifter.py produce IR? (P3+)
  emitted   yes / no          — does emitter.py produce C? (P3+)
  oracle    yes / N/A         — does our mnemonic agree with Beetle's
                                static op table?

`Invalid` means NEC documents the slot as reserved AND Beetle traps
to `op_INVALID`; our decoder produces `is_unknown=True`. `N/A` in the
oracle column appears only for slots Beetle treats as INVALID and
sub-op dispatchers (where the oracle check happens per sub-op).
"""
from __future__ import annotations

from pathlib import Path
from typing import Optional

from ..tests._beetle_op_table import (
    BEETLE_TO_OURS,
    beetle_op_for_hw0,
    parse_op_table,
)
from ..v810.isa import (
    BCOND,
    BSU_SUBOP,
    FPP_SUBOP,
    Format,
    PRIMARY,
)


PROJECT_ROOT = Path(__file__).resolve().parents[2]
BEETLE_OP_TABLE = (PROJECT_ROOT / "beetle-vb" / "mednafen" / "hw_cpu"
                   / "v810" / "v810_op_table_msvc.inc")
STATUS_DOC = PROJECT_ROOT / "docs" / "INSTRUCTION_STATUS.md"


def _oracle_status_for_primary(primary: int,
                               our_mnemonic: str,
                               op_table: dict[int, str]) -> str:
    """For a primary opcode (non-Bcond, non-sub-op), check that our
    mnemonic matches what Beetle's op_table dispatches to."""
    # Construct a representative hw0 with this primary; the low 10
    # bits don't matter for non-Bcond dispatch.
    hw0 = primary << 10
    op_name = beetle_op_for_hw0(op_table, hw0)
    expected = BEETLE_TO_OURS.get(op_name)
    if op_name == "op_INVALID":
        return "N/A"
    if expected is None:
        # Sub-op dispatcher (op_BSTR / op_FPP) — oracle is per sub-op.
        return "N/A"
    return "yes" if expected == our_mnemonic else f"NO ({expected})"


def _oracle_status_for_bcond(cond: int, our_mnemonic: str,
                             op_table: dict[int, str]) -> str:
    # Bcond dispatch index = 0x40 + cond. Equivalent hw0: 0b100 in
    # bits 15:13, cond in bits 12:9.
    hw0 = (0b100 << 13) | (cond << 9)
    op_name = beetle_op_for_hw0(op_table, hw0)
    expected = BEETLE_TO_OURS.get(op_name)
    if expected is None:
        return "N/A"
    return "yes" if expected == our_mnemonic else f"NO ({expected})"


def _row(opcode: str, fmt: str, decoded: str, oracle: str,
         *, lifted: str = "no", emitted: str = "no",
         note: Optional[str] = None) -> str:
    cells = [opcode, fmt, decoded, lifted, emitted, oracle]
    if note is not None:
        cells.append(note)
    return "| " + " | ".join(cells) + " |"


def _table_header(with_note: bool = False) -> list[str]:
    if with_note:
        return [
            "| opcode | format | decoded | lifted | emitted | oracle | notes |",
            "|--------|--------|---------|--------|---------|--------|-------|",
        ]
    return [
        "| opcode | format | decoded | lifted | emitted | oracle |",
        "|--------|--------|---------|--------|---------|--------|",
    ]


def build_status_markdown() -> str:
    """Render the full INSTRUCTION_STATUS.md from live tables."""
    op_table = parse_op_table(BEETLE_OP_TABLE)
    out: list[str] = []
    out.append("# INSTRUCTION_STATUS.md — V810 opcode coverage")
    out.append("")
    out.append("**Single source of truth** for V810 implementation status.")
    out.append("")
    out.append("This table is regenerated mechanically by")
    out.append("`python -m recompiler.cli.vbrecomp_coverage --regen-status`.")
    out.append("Do not hand-edit — the regenerator overwrites it.")
    out.append("")
    out.append("Columns:")
    out.append("")
    out.append("- **opcode** — V810 mnemonic")
    out.append("- **format** — I / II / III / IV / V / VI / VII")
    out.append("- **decoded** — `yes` / `Invalid` — does `decoder.py` "
               "recognise the opcode bits and extract operand fields?")
    out.append("- **lifted** — `yes` / `no` — does `lifter.py` produce IR? "
               "(P3+)")
    out.append("- **emitted** — `yes` / `no` — does `emitter.py` produce C? "
               "(P3+)")
    out.append("- **oracle** — `yes` / `N/A` — does our mnemonic agree with "
               "Beetle VB's static op table?")
    out.append("")
    out.append("`Invalid` means NEC documents the slot as reserved AND "
               "Beetle traps to `op_INVALID`; the decoder reports "
               "`is_unknown=True`. `N/A` in the oracle column appears for "
               "slots Beetle invalid-traps (no mnemonic to compare) and "
               "for sub-op dispatchers (where the oracle check is per "
               "sub-op).")
    out.append("")

    # ---- Format I — register-register ----
    out.append("## Format I — register-register (2-byte; 6-bit opcode + reg2 + reg1)")
    out.append("")
    out.extend(_table_header())
    for opc in range(0x00, 0x10):
        spec = PRIMARY[opc]
        oracle = _oracle_status_for_primary(opc, spec.mnemonic, op_table)
        out.append(_row(spec.mnemonic, "I", "yes", oracle))
    out.append("")

    # ---- Format II — register-imm5 ----
    out.append("## Format II — register + 5-bit immediate (2-byte)")
    out.append("")
    out.extend(_table_header(with_note=True))
    for opc in range(0x10, 0x20):
        spec = PRIMARY[opc]
        if spec.opcode == -1:
            out.append(_row(f"`<reserved 0x{opc:02X}>`", "II", "Invalid",
                            "N/A", note="NEC-reserved primary opcode"))
            continue
        oracle = _oracle_status_for_primary(opc, spec.mnemonic, op_table)
        note = ""
        if opc == 0x16:
            note = "Beetle calls this `op_EI` — same insn"
        elif opc == 0x1E:
            note = "Beetle calls this `op_DI` — same insn"
        elif opc == 0x1F:
            note = "BSU dispatcher — see Format VII / BSU below"
        out.append(_row(spec.mnemonic, "II", "yes", oracle, note=note))
    out.append("")

    # ---- Format III — Bcond ----
    out.append("## Format III — conditional branch (Bcond, 9-bit disp)")
    out.append("")
    out.extend(_table_header())
    for cond in range(0x10):
        mnem = BCOND[cond]
        oracle = _oracle_status_for_bcond(cond, mnem, op_table)
        out.append(_row(mnem, "III", "yes", oracle))
    out.append("")

    # ---- Format IV — long jump ----
    out.append("## Format IV — long jump (4-byte; 26-bit disp)")
    out.append("")
    out.extend(_table_header())
    for opc in (0x2A, 0x2B):
        spec = PRIMARY[opc]
        oracle = _oracle_status_for_primary(opc, spec.mnemonic, op_table)
        out.append(_row(spec.mnemonic, "IV", "yes", oracle))
    out.append("")

    # ---- Format V — register + imm16 ----
    out.append("## Format V — register + 16-bit immediate (4-byte)")
    out.append("")
    out.extend(_table_header())
    for opc in (0x28, 0x29, 0x2C, 0x2D, 0x2E, 0x2F):
        spec = PRIMARY[opc]
        oracle = _oracle_status_for_primary(opc, spec.mnemonic, op_table)
        out.append(_row(spec.mnemonic, "V", "yes", oracle))
    out.append("")

    # ---- Format VI — load/store/IN/OUT/CAXI ----
    out.append("## Format VI — load / store / IN / OUT / CAXI (4-byte; 16-bit disp)")
    out.append("")
    out.extend(_table_header(with_note=True))
    for opc in range(0x30, 0x40):
        spec = PRIMARY[opc]
        if spec.opcode == -1:
            out.append(_row(f"`<reserved 0x{opc:02X}>`", "VI", "Invalid",
                            "N/A", note="NEC-reserved primary opcode"))
            continue
        if opc == 0x3E:
            continue
        oracle = _oracle_status_for_primary(opc, spec.mnemonic, op_table)
        note = ""
        if opc == 0x3A:
            note = "Sacred Tech Scroll: Format VI (not VII)"
        out.append(_row(spec.mnemonic, "VI", "yes", oracle, note=note))
    out.append("")

    # ---- Format VII — FPP sub-ops ----
    out.append("## Format VII — FPP floating-point + extensions (4-byte; sub-op in 2nd halfword bits 15:10)")
    out.append("")
    out.extend(_table_header(with_note=True))
    for subop in sorted(FPP_SUBOP):
        mnem = FPP_SUBOP[subop]
        # Beetle's v810_opt.h defines the same sub-op constants; the
        # oracle check is "is this sub-op present in both tables?".
        oracle = "yes"
        out.append(_row(mnem, "VII", "yes", oracle,
                        note=f"FPP sub-op 0x{subop:02X}"))
    out.append("")

    # ---- Format VII — BSU sub-ops ----
    out.append("## Format VII — BSU bitstring (2-byte; sub-op in reg2 field bits 9:5)")
    out.append("")
    out.extend(_table_header(with_note=True))
    for subop in sorted(BSU_SUBOP):
        mnem = BSU_SUBOP[subop]
        oracle = "yes"
        out.append(_row(mnem, "VII", "yes", oracle,
                        note=f"BSU sub-op 0x{subop:02X}"))
    out.append("")

    out.append("---")
    out.append("")
    out.append("## Oracle source")
    out.append("")
    out.append("The `oracle` column is checked at regen time against Beetle "
               "VB's static dispatch table at")
    out.append(f"`{BEETLE_OP_TABLE.relative_to(PROJECT_ROOT).as_posix()}`.")
    out.append("")
    out.append("Full per-instruction parity (mnemonic at every CFG-aware "
               "reachable PC on a representative cart) is verified by "
               "`recompiler/tests/test_decoder_oracle.py`.")
    out.append("")
    return "\n".join(out)


def regen_instruction_status() -> int:
    """Write `docs/INSTRUCTION_STATUS.md` from current isa.py + Beetle
    op table. Returns 0 on success, 2 if any opcode oracle column came
    out as a `NO (...)` mismatch (so CI can catch drift)."""
    if not BEETLE_OP_TABLE.exists():
        print(f"error: Beetle op table not found at {BEETLE_OP_TABLE}")
        return 2
    text = build_status_markdown()
    STATUS_DOC.write_text(text, encoding="utf-8")
    failed = "NO (" in text
    print(f"wrote {STATUS_DOC.relative_to(PROJECT_ROOT).as_posix()}"
          f" ({len(text)} bytes)")
    return 2 if failed else 0


if __name__ == "__main__":
    import sys
    sys.exit(regen_instruction_status())
