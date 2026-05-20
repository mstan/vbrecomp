"""Memory map classification tests."""
from __future__ import annotations

import unittest

from recompiler.rom.memory_map import (
    PHYS_MASK,
    RESET_VECTOR,
    Region,
    classify,
    fold,
    rom_offset,
)


class TestClassify(unittest.TestCase):
    def test_each_region_base_classifies_correctly(self):
        cases = [
            (0x00000000, Region.VIP),
            (0x01000000, Region.VSU),
            (0x02000028, Region.MISC),         # pad register
            (0x03000000, Region.RESERVED_3),
            (0x04000000, Region.CART_EXP),
            (0x05000FFF, Region.WRAM),
            (0x06000100, Region.CART_RAM),
            (0x07FFFFF0, Region.CART_ROM),
        ]
        for addr, expected in cases:
            with self.subTest(addr=addr):
                self.assertEqual(classify(addr).region, expected)

    def test_high_bits_fold_27_bit(self):
        # 0xFFFFFFF0 must fold to 0x07FFFFF0 (cart ROM reset vector)
        self.assertEqual(fold(RESET_VECTOR), 0x07FFFFF0)
        self.assertEqual(classify(RESET_VECTOR).region, Region.CART_ROM)

    def test_phys_mask_is_27_bit(self):
        self.assertEqual(PHYS_MASK, 0x07FFFFFF)


class TestRomOffset(unittest.TestCase):
    def test_reset_vector_2mb_rom(self):
        rom_size = 2 * 1024 * 1024
        off = rom_offset(RESET_VECTOR, rom_size)
        # reset vector folds to 0x07FFFFF0, offset = 0x07FFFFF0 - 0x07000000 = 0xFFFFF0
        # mask with rom_size-1 (since rom_size is a power of 2)
        expected = 0xFFFFF0 & (rom_size - 1)
        self.assertEqual(off, expected)

    def test_non_cart_rom_returns_none(self):
        self.assertIsNone(rom_offset(0x05001000, 1024 * 1024))  # WRAM address

    def test_mirroring_small_rom(self):
        # A 128 KB ROM mirrored 8x across the 16 MB cart-ROM window
        rom_size = 128 * 1024
        # 0x07020000 is 0x20000 bytes past cart ROM base; mirroring puts
        # that offset back to 0 in a 128 KB ROM
        self.assertEqual(rom_offset(0x07020000, rom_size), 0)


if __name__ == "__main__":
    unittest.main()
