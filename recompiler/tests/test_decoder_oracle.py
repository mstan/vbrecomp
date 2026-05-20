"""L1 oracle parity — decoder mnemonic vs Beetle VB's static op table.

For every instruction reachable from the reset trampoline via the
CFG-aware walk on a representative cart, the test:

  1. Looks up Beetle's `op_NAME` for the instruction's first
     halfword via the parsed `v810_op_table_msvc.inc` dispatch table.
  2. Translates that op_NAME to the mnemonic our decoder is expected
     to produce (matching `recompiler/v810/isa.py`).
  3. Resolves BSU/FPP sub-opcodes by consulting our own tables —
     Beetle's `v810_opt.h` defines exactly the same sub-op values
     (verified by inspection during P2), so a name-table mismatch
     here is a real decoder bug, not a mapping mismatch.
  4. For instructions Beetle flags `op_INVALID`, asserts our decoder
     marks them `is_unknown=True`.
  5. For everything else, asserts our mnemonic equals Beetle's
     expected mnemonic.

The test is *static*: it does not bring up `vb-beetle.exe` nor a
TCP server. The op-table file is the authoritative oracle — it is
the same source the libretro core uses at runtime. This gives full
mnemonic parity without the SDL window / audio / libretro bring-up
that the P2.5 visual milestone requires.

ROMs the test exercises:

  - `roms/marios_tennis.vb` — primary P2 target (smallest 4 Mbit cart)
  - `roms/v_tetris.vb`      — second commercial cart, when present

Each ROM is gated by file existence; the test reports SKIPPED rather
than failing if a ROM is absent (e.g. on a fresh checkout that doesn't
have the binaries copied over).
"""
from __future__ import annotations

import unittest
from pathlib import Path
from typing import Dict, List, Optional

from recompiler.v810.analysis import (
    RomImage,
    cfg_walk_with_table_resolution,
    trace_reset_trampoline,
)
from recompiler.v810.decoder import decode_at
from recompiler.v810.isa import BSU_SUBOP, FPP_SUBOP

from recompiler.tests._beetle_op_table import (
    beetle_op_for_hw0,
    expected_mnemonic_for_op,
    parse_op_table,
)


# Resolve project layout from this file's location. The test imports
# Beetle's dispatch table from the live submodule.
PROJECT_ROOT = Path(__file__).resolve().parents[2]
BEETLE_OP_TABLE = PROJECT_ROOT / "beetle-vb" / "mednafen" / "hw_cpu" / "v810" / "v810_op_table_msvc.inc"


def _expected_for_pc(rom: RomImage, op_table: Dict[int, str], pc: int
                     ) -> tuple[Optional[str], str]:
    """Return (expected_mnemonic, op_name) for the instruction at pc.

    `expected_mnemonic` is None when Beetle would treat the encoding
    as invalid (op_INVALID) — the caller asserts our decoder flags
    `is_unknown` in that case.
    """
    off = rom.va_to_offset(pc)
    assert off is not None and off + 2 <= rom.rom_size
    hw0 = rom.data[off] | (rom.data[off + 1] << 8)
    op_name = beetle_op_for_hw0(op_table, hw0)

    # Resolve BSU sub-op: Beetle dispatches inside op_BSTR on the reg2
    # field. Our decoder uses the same `BSU_SUBOP` table; mismatches
    # are decoder bugs in either the primary classification or the
    # sub-op lookup.
    if op_name == "op_BSTR":
        subop = (hw0 >> 5) & 0x1F
        if subop in BSU_SUBOP:
            return BSU_SUBOP[subop], op_name
        return None, op_name

    # Resolve FPP sub-op: lives in bits 15:10 of the second halfword.
    if op_name == "op_FPP":
        if off + 4 > rom.rom_size:
            return None, op_name
        hw1 = rom.data[off + 2] | (rom.data[off + 3] << 8)
        subop = (hw1 >> 10) & 0x3F
        if subop in FPP_SUBOP:
            return FPP_SUBOP[subop], op_name
        return None, op_name

    return expected_mnemonic_for_op(op_name), op_name


def _verify_cart_against_oracle(test: unittest.TestCase,
                                rom_path: Path) -> tuple[int, int]:
    """Walk a cart CFG-aware and compare each decoded mnemonic against
    Beetle's table. Returns (visited_instr_count, mismatch_count).

    Mismatches surface as `self.fail()` with the offending PC, our
    mnemonic, and Beetle's op_NAME — so a regression points directly
    at the encoding that broke.
    """
    op_table = parse_op_table(BEETLE_OP_TABLE)
    img = RomImage.from_bytes(rom_path.read_bytes())
    entry = trace_reset_trampoline(img)
    test.assertIsNotNone(entry,
                         f"trampoline tracer failed on {rom_path.name}")
    walk = cfg_walk_with_table_resolution(img, [entry])
    test.assertGreater(len(walk.visited), 0,
                       f"CFG walk produced no visited PCs on {rom_path.name}")

    mismatches: List[str] = []
    for pc in sorted(walk.visited.keys()):
        off = img.va_to_offset(pc)
        if off is None:
            continue
        ins = decode_at(img.data, off, pc=pc)
        expected, op_name = _expected_for_pc(img, op_table, pc)

        if expected is None:
            # Beetle would invalid-trap; we'd better mark it unknown.
            if not ins.is_unknown:
                mismatches.append(
                    f"  pc=0x{pc:08X} hw0=0x{ins.raw[:2].hex()}  "
                    f"ours='{ins.mnemonic}' (NOT marked unknown), "
                    f"Beetle='{op_name}' (would invalid-trap)"
                )
        else:
            if ins.is_unknown:
                mismatches.append(
                    f"  pc=0x{pc:08X} hw0=0x{ins.raw[:2].hex()}  "
                    f"ours='{ins.mnemonic}' (marked unknown), "
                    f"Beetle expects '{expected}' (op_name='{op_name}')"
                )
            elif ins.mnemonic != expected:
                mismatches.append(
                    f"  pc=0x{pc:08X} hw0=0x{ins.raw[:2].hex()}  "
                    f"ours='{ins.mnemonic}' vs Beetle='{expected}' "
                    f"(op_name='{op_name}')"
                )

    if mismatches:
        # Cap the report length so a wholesale mismatch doesn't drown
        # the diagnostic output.
        report = "\n".join(mismatches[:40])
        if len(mismatches) > 40:
            report += f"\n  ... and {len(mismatches) - 40} more"
        test.fail(
            f"L1 oracle parity failed on {rom_path.name}: "
            f"{len(mismatches)} mnemonic mismatches "
            f"over {len(walk.visited)} reachable instructions.\n{report}"
        )

    return len(walk.visited), len(mismatches)


class TestBeetleOpTableParsing(unittest.TestCase):
    def test_parser_extracts_primary_opcodes(self):
        if not BEETLE_OP_TABLE.exists():
            self.skipTest(f"beetle-vb op table missing: {BEETLE_OP_TABLE}")
        table = parse_op_table(BEETLE_OP_TABLE)
        # Spot-check a handful of canonical primaries.
        self.assertEqual(table[0], "op_MOV")     # case 0,1 → op_MOV
        self.assertEqual(table[1], "op_MOV")
        self.assertEqual(table[12], "op_JMP")
        self.assertEqual(table[44], "op_EI")     # primary 0x16 → Beetle EI
        self.assertEqual(table[60], "op_DI")     # primary 0x1E → Beetle DI
        self.assertEqual(table[116], "op_CAXI")  # primary 0x3A → Beetle CAXI
        self.assertEqual(table[124], "op_FPP")   # primary 0x3E → FPP
        self.assertEqual(table[126], "op_OUT_W") # primary 0x3F → OUT.W

    def test_bcond_cond_slots_distinct(self):
        if not BEETLE_OP_TABLE.exists():
            self.skipTest(f"beetle-vb op table missing: {BEETLE_OP_TABLE}")
        table = parse_op_table(BEETLE_OP_TABLE)
        self.assertEqual(table[64], "op_BV")     # cond 0x0
        self.assertEqual(table[69], "op_BR")     # cond 0x5
        self.assertEqual(table[77], "op_NOP")    # cond 0xD
        self.assertEqual(table[79], "op_BGT")    # cond 0xF

    def test_reserved_primary_absent_from_table(self):
        # Beetle maps reserved primary opcodes (0x1B, 0x32, 0x36) to
        # op_INVALID — verifiable by direct lookup.
        if not BEETLE_OP_TABLE.exists():
            self.skipTest(f"beetle-vb op table missing: {BEETLE_OP_TABLE}")
        table = parse_op_table(BEETLE_OP_TABLE)
        for primary in (0x1B, 0x32, 0x36):
            for low_bit in (0, 1):
                idx = (primary << 1) | low_bit
                self.assertEqual(table[idx], "op_INVALID",
                                 f"primary 0x{primary:02X} idx {idx}")


class TestL1OracleParity(unittest.TestCase):
    """Per-instruction decoder parity against the Beetle op table."""

    def _check_cart(self, name: str) -> None:
        if not BEETLE_OP_TABLE.exists():
            self.skipTest(f"beetle-vb op table missing: {BEETLE_OP_TABLE}")
        rom_path = PROJECT_ROOT / "roms" / name
        if not rom_path.exists():
            self.skipTest(f"ROM missing: {rom_path}")
        visited, mismatches = _verify_cart_against_oracle(self, rom_path)
        # Provide a positive signal so it's visible from -v output.
        print(f"\n  [oracle] {name}: {visited} insns checked, "
              f"{mismatches} mismatches")

    def test_marios_tennis(self) -> None:
        self._check_cart("marios_tennis.vb")

    def test_v_tetris(self) -> None:
        self._check_cart("v_tetris.vb")


if __name__ == "__main__":
    unittest.main()
