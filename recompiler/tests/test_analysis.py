"""Tests for v810/analysis.py — reset trampoline tracer, CFG walk,
function discovery, basic-block construction.

Synthetic mini-ROMs only; nothing in here loads a commercial cart.
The L1 oracle test (test_decoder_oracle.py) is the real-cart side.
"""
from __future__ import annotations

import unittest

from recompiler.v810.analysis import (
    BasicBlock,
    CART_BANK_BASE,
    RESET_VECTOR,
    RomImage,
    build_cfg,
    cfg_walk_from_seeds,
    discover_functions,
    trace_reset_trampoline,
)


# --- Encoding helpers (duplicated narrowly from test_decoder; analysis
#     tests use the same set of synthetic encoders rather than importing
#     across test modules) ---

def _enc_fmt_i(opcode6: int, reg2: int, reg1: int) -> bytes:
    hw = (opcode6 << 10) | (reg2 << 5) | reg1
    return bytes([hw & 0xFF, (hw >> 8) & 0xFF])

def _enc_fmt_ii(opcode6: int, reg2: int, imm5: int) -> bytes:
    return _enc_fmt_i(opcode6, reg2, imm5 & 0x1F)

def _enc_bcond(cond: int, disp9: int) -> bytes:
    hw = (0b100 << 13) | ((cond & 0xF) << 9) | (disp9 & 0x1FF)
    return bytes([hw & 0xFF, (hw >> 8) & 0xFF])

def _enc_fmt_iv(opcode6: int, disp26: int) -> bytes:
    raw = disp26 & ((1 << 26) - 1)
    hw0 = (opcode6 << 10) | ((raw >> 16) & 0x3FF)
    hw1 = raw & 0xFFFF
    return bytes([hw0 & 0xFF, (hw0 >> 8) & 0xFF, hw1 & 0xFF, (hw1 >> 8) & 0xFF])

def _enc_fmt_v(opcode6: int, reg2: int, reg1: int, imm16: int) -> bytes:
    hw0 = (opcode6 << 10) | (reg2 << 5) | reg1
    hw1 = imm16 & 0xFFFF
    return bytes([hw0 & 0xFF, (hw0 >> 8) & 0xFF, hw1 & 0xFF, (hw1 >> 8) & 0xFF])

def _movhi(rD: int, rS: int, imm16: int) -> bytes:
    return _enc_fmt_v(0x2F, rD, rS, imm16)

def _movea(rD: int, rS: int, imm16: int) -> bytes:
    return _enc_fmt_v(0x28, rD, rS, imm16)

def _jmp(rX: int) -> bytes:
    return _enc_fmt_i(0x06, 0, rX)

def _jal(disp26: int) -> bytes:
    return _enc_fmt_iv(0x2B, disp26)

def _jr(disp26: int) -> bytes:
    return _enc_fmt_iv(0x2A, disp26)

def _ret() -> bytes:
    return _jmp(31)

def _halt() -> bytes:
    return _enc_fmt_ii(0x1A, 0, 0)

def _br(disp9: int) -> bytes:
    """BR — Bcond with cond=0x5 (always); no fall-through."""
    return _enc_bcond(0x5, disp9)

def _be(disp9: int) -> bytes:
    """BE — conditional branch on equal; has fall-through."""
    return _enc_bcond(0x2, disp9)

def _add_rr(rD: int, rS: int) -> bytes:
    return _enc_fmt_i(0x01, rD, rS)

def _mov_rr(rD: int, rS: int) -> bytes:
    return _enc_fmt_i(0x00, rD, rS)


# A 16 KB cart is plenty for these tests; everything fits in the first
# few hundred bytes plus the trampoline at the end.
TEST_ROM_SIZE = 0x4000


def _blank_rom() -> bytearray:
    """A clean 16 KB ROM image filled with 0x00 (decodes as MOV r0,r0).

    The trampoline at the very end is the caller's responsibility; the
    helpers below write into specific offsets.
    """
    return bytearray(TEST_ROM_SIZE)


def _install_simple_trampoline(rom: bytearray, *, entry_va: int) -> None:
    """Write the canonical MOVHI/MOVEA/JMP r11 trampoline that lands
    at `entry_va`. The last 16 bytes of `rom` get overwritten."""
    high = (entry_va >> 16) & 0xFFFF
    low = entry_va & 0xFFFF
    # MOVEA's imm16 is sign-extended; if the low half's high bit is
    # set, MOVEA subtracts effectively — compensate by adding 1 to
    # the high half.
    if low & 0x8000:
        high = (high + 1) & 0xFFFF
        signed_low = low - 0x10000
    else:
        signed_low = low
    tramp_off = TEST_ROM_SIZE - 16
    rom[tramp_off + 0:tramp_off + 4] = _movhi(11, 0, high)
    rom[tramp_off + 4:tramp_off + 8] = _movea(11, 11, signed_low & 0xFFFF)
    rom[tramp_off + 8:tramp_off + 10] = _jmp(11)
    assert ((high << 16) + (signed_low if signed_low < 0x8000
                            else signed_low - 0x10000)) & 0xFFFFFFFF == entry_va


# ----------------------------------------------------------------------


class TestRomImageMirroring(unittest.TestCase):
    def test_bank7_base_maps_offset_zero(self):
        rom = RomImage.from_bytes(b"\x00" * 0x400)
        self.assertEqual(rom.va_to_offset(0x07000000), 0)

    def test_reset_vector_folds_to_end_minus_16(self):
        # 1 KiB cart: reset vector should map to offset 0x3F0.
        rom = RomImage.from_bytes(b"\x00" * 0x400)
        self.assertEqual(rom.va_to_offset(0xFFFFFFF0), 0x3F0)

    def test_outside_bank7_is_none(self):
        rom = RomImage.from_bytes(b"\x00" * 0x400)
        # Bank 5 (WRAM) is not the cart.
        self.assertIsNone(rom.va_to_offset(0x05000000))
        self.assertIsNone(rom.va_to_offset(0x00000000))

    def test_mirror_wraps_within_bank7(self):
        rom = RomImage.from_bytes(b"\xAB" * 0x100)
        # va_bank+rom_size mirrors to the same offset.
        self.assertEqual(rom.va_to_offset(0x07000100), 0)
        self.assertEqual(rom.va_to_offset(0x07000200), 0)

    def test_non_power_of_two_clamps(self):
        # 384-byte cart (not power of two) — accesses past the end
        # return None instead of wrapping.
        rom = RomImage.from_bytes(b"\x00" * 0x180)
        self.assertEqual(rom.va_to_offset(0x07000000), 0)
        self.assertIsNone(rom.va_to_offset(0x07000200))


class TestTraceResetTrampoline(unittest.TestCase):
    def test_movhi_movea_jmp_resolves_entry(self):
        rom = _blank_rom()
        _install_simple_trampoline(rom, entry_va=0xFFF80120)
        img = RomImage.from_bytes(bytes(rom))
        entry = trace_reset_trampoline(img)
        self.assertEqual(entry, 0xFFF80120)

    def test_movhi_movea_jmp_with_low_high_bit(self):
        # entry_va has 0x8000 set in low half — exercises the
        # MOVHI+1 / MOVEA-as-subtract compensation.
        rom = _blank_rom()
        _install_simple_trampoline(rom, entry_va=0xFFF88040)
        img = RomImage.from_bytes(bytes(rom))
        self.assertEqual(trace_reset_trampoline(img), 0xFFF88040)

    def test_direct_jr_trampoline(self):
        # A homebrew-style "JR to entry" tucked into the last 16 bytes.
        rom = _blank_rom()
        # We need JR's target = some entry VA. JR pc-relative; the
        # JR insn is at VA RESET_VECTOR (0xFFFFFFF0), and we want it
        # to land at, e.g., 0xFFFFE000. disp = entry - pc.
        disp = (0xFFFFE000 - RESET_VECTOR) & 0xFFFFFFFF
        # Convert to signed 26-bit; we know the difference fits.
        rom[TEST_ROM_SIZE - 16:TEST_ROM_SIZE - 12] = _jr(disp - 0x100000000
                                                        if disp & 0x02000000
                                                        else disp)
        img = RomImage.from_bytes(bytes(rom))
        self.assertEqual(trace_reset_trampoline(img), 0xFFFFE000)

    def test_unrecognized_trampoline_returns_none(self):
        # The trampoline opens with an FPP add — outside the
        # whitelist. We expect None, not a guess.
        rom = _blank_rom()
        # FPP ADDF.S r0, r0 (4 bytes), subop=0x04.
        hw0 = (0x3E << 10) | (0 << 5) | 0
        hw1 = (0x04 << 10)
        rom[TEST_ROM_SIZE - 16:TEST_ROM_SIZE - 12] = bytes([
            hw0 & 0xFF, (hw0 >> 8) & 0xFF,
            hw1 & 0xFF, (hw1 >> 8) & 0xFF,
        ])
        img = RomImage.from_bytes(bytes(rom))
        self.assertIsNone(trace_reset_trampoline(img))


class TestCFGWalk(unittest.TestCase):
    def test_walks_jal_branch_and_return(self):
        rom = _blank_rom()
        # Entry at offset 0x20 (VA 0x07000020).
        # JAL +0x40 -> VA 0x07000060 (offset 0x60)
        rom[0x20:0x24] = _jal(0x40)
        # BE (conditional) +0x10 -> target offset 0x34 (disp = 0x34-0x24)
        rom[0x24:0x26] = _be(0x10)
        # HALT — reached on the fall-through (branch not taken).
        rom[0x26:0x28] = _halt()
        # Branch target at 0x34: ADD r2,r1; JMP r31
        rom[0x34:0x36] = _add_rr(2, 1)
        rom[0x36:0x38] = _ret()
        # Callee at 0x60: MOV r2,r1; JMP r31
        rom[0x60:0x62] = _mov_rr(2, 1)
        rom[0x62:0x64] = _ret()

        img = RomImage.from_bytes(bytes(rom))
        walk = cfg_walk_from_seeds(img, [0x07000020])

        expected = {
            0x07000020, 0x07000024, 0x07000026,
            0x07000034, 0x07000036,
            0x07000060, 0x07000062,
        }
        self.assertEqual(set(walk.visited.keys()), expected)
        self.assertEqual(walk.call_targets, {0x07000060})
        self.assertEqual(walk.indirect_jumps, {})
        self.assertEqual(walk.unknown_pcs, set())

    def test_jr_overwrites_pc_no_fallthrough(self):
        rom = _blank_rom()
        # Entry at 0x20: JR +0x20 (to 0x40); the bytes immediately
        # after the JR must NOT be visited (no fall-through past JR).
        rom[0x20:0x24] = _jr(0x20)
        # If the walker incorrectly fell through, it would decode
        # whatever's at 0x24 — install a halt-like marker there that
        # would show up if visited.
        rom[0x24:0x26] = _halt()
        rom[0x40:0x42] = _ret()
        img = RomImage.from_bytes(bytes(rom))
        walk = cfg_walk_from_seeds(img, [0x07000020])
        self.assertIn(0x07000020, walk.visited)
        self.assertIn(0x07000040, walk.visited)
        self.assertNotIn(0x07000024, walk.visited)

    def test_indirect_jump_stops_walk(self):
        rom = _blank_rom()
        # Entry: JMP r1 (indirect — r1 not pre-loaded so the walk
        # cannot resolve the target).
        rom[0x20:0x22] = _jmp(1)
        # Marker that must not be visited:
        rom[0x22:0x24] = _ret()
        img = RomImage.from_bytes(bytes(rom))
        walk = cfg_walk_from_seeds(img, [0x07000020])
        self.assertIn(0x07000020, walk.visited)
        self.assertNotIn(0x07000022, walk.visited)
        self.assertEqual(walk.indirect_jumps, {0x07000020: 1})

    def test_branch_target_outside_cart_logged_not_followed(self):
        rom = _blank_rom()
        # JR with a disp landing in bank 6 (cart-RAM). The decoder
        # computes branch_target outside bank 7; the walker logs it
        # and stops. Target 0x06000000 from pc 0x07000020 is disp
        # -0x01000020, which is inside the signed-26 range.
        rom[0x20:0x24] = _jr(-0x01000020)
        img = RomImage.from_bytes(bytes(rom))
        walk = cfg_walk_from_seeds(img, [0x07000020])
        self.assertIn(0x07000020, walk.visited)
        self.assertIn(0x06000000, walk.off_cart_pcs)

    def test_revisit_short_circuits(self):
        # A backwards branch creates a loop; the walk must terminate.
        rom = _blank_rom()
        # pc 0x20: ADD r2,r1
        rom[0x20:0x22] = _add_rr(2, 1)
        # pc 0x22: BR -2 → target = 0x20 (back to ADD)
        rom[0x22:0x24] = _br(-2)
        img = RomImage.from_bytes(bytes(rom))
        walk = cfg_walk_from_seeds(img, [0x07000020])
        # Both insns visited exactly once.
        self.assertEqual(set(walk.visited.keys()),
                         {0x07000020, 0x07000022})


class TestDiscoverFunctions(unittest.TestCase):
    def test_trampoline_entry_plus_callee(self):
        rom = _blank_rom()
        # Entry at VA 0x07000020.
        _install_simple_trampoline(rom, entry_va=CART_BANK_BASE + 0x20)
        # Entry function: JAL +0x40, HALT.
        rom[0x20:0x24] = _jal(0x40)
        rom[0x24:0x26] = _halt()
        # Callee at offset 0x60: MOV r2,r1; JMP r31.
        rom[0x60:0x62] = _mov_rr(2, 1)
        rom[0x62:0x64] = _ret()

        img = RomImage.from_bytes(bytes(rom))
        fns = discover_functions(img)
        starts = sorted(f.start_pc for f in fns)
        # We now also expect the trampoline itself as a function — it
        # lives at the last 16 bytes of the cart, mirrored from
        # 0xFFFFFFF0. discover_functions seeds from RESET_VECTOR so
        # the trampoline insns are walked and bucketed.
        self.assertIn(RESET_VECTOR, starts)
        self.assertIn(0x07000020, starts)
        self.assertIn(0x07000060, starts)
        # Entry function spans 0x20..0x26 (JAL+HALT = 4+2 = 6 bytes).
        entry_fn = next(f for f in fns if f.start_pc == 0x07000020)
        self.assertEqual(entry_fn.end_pc, 0x07000026)
        # Callee spans 0x60..0x64.
        callee = next(f for f in fns if f.start_pc == 0x07000060)
        self.assertEqual(callee.end_pc, 0x07000064)

    def test_unrecognized_trampoline_walks_what_it_can(self):
        # An all-zero blank ROM decodes as `MOV r0, r0` (Format I, opcode 0)
        # everywhere. Walking from RESET_VECTOR yields a long fall-through
        # chain that never terminates within the buffer, so the walker
        # eventually falls off the cart — but it does produce a non-zero
        # function set, which is the contract we care about (the codegen
        # downstream will fail to compile this trash, which is fine —
        # it's not a real cart).
        rom = _blank_rom()
        img = RomImage.from_bytes(bytes(rom))
        fns = discover_functions(img)
        # At minimum the trampoline location appears.
        self.assertTrue(any(f.start_pc == RESET_VECTOR for f in fns))


class TestBuildCFG(unittest.TestCase):
    def test_two_blocks_from_branch(self):
        rom = _blank_rom()
        # Function at VA 0x07000020 .. 0x07000028:
        #   0x20: BR +4   → target 0x24
        #   0x22: HALT    (skipped if branch taken)
        #   0x24: ADD r2,r1
        #   0x26: JMP r31
        rom[0x20:0x22] = _br(4)
        rom[0x22:0x24] = _halt()
        rom[0x24:0x26] = _add_rr(2, 1)
        rom[0x26:0x28] = _ret()
        img = RomImage.from_bytes(bytes(rom))

        from recompiler.v810.analysis import FunctionRange
        fn = FunctionRange("fn_test", 0x07000020, 0x07000028)
        cfg = build_cfg(fn, img)

        # Expect leaders at: 0x20 (entry), 0x22 (fall-through after BR),
        # 0x24 (branch target). That's 3 leaders → 3 blocks.
        starts = sorted(b.start_pc for b in cfg.blocks)
        self.assertEqual(starts, [0x07000020, 0x07000022, 0x07000024])
        # Entry block (0x20): the BR succeeds to both 0x22 (fall) and
        # 0x24 (taken).
        entry_block = next(b for b in cfg.blocks
                           if b.start_pc == 0x07000020)
        succ_starts = {cfg.blocks[s].start_pc for s in entry_block.succ}
        self.assertEqual(succ_starts, {0x07000022, 0x07000024})


if __name__ == "__main__":
    unittest.main()
