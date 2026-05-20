"""ROM header parser tests."""
from __future__ import annotations

import hashlib
import unittest

from recompiler.rom.header import (
    HEADER_SIZE,
    MIN_ROM_SIZE,
    is_known_size,
    parse_header,
)


def _make_rom(rom_size: int, *, title: str = "VBRECOMP_TEST_ROM",
              maker: str = "01", game: str = "TEST",
              version: int = 0x07) -> bytes:
    body = bytearray(b"\x00" * (rom_size - HEADER_SIZE))
    header = bytearray(b"\x00" * HEADER_SIZE)

    title_bytes = title.encode("ascii")
    if len(title_bytes) > 0x14:
        title_bytes = title_bytes[:0x14]
    header[0:len(title_bytes)] = title_bytes
    # pad title with spaces to 20 bytes
    for i in range(len(title_bytes), 0x14):
        header[i] = 0x20  # space

    maker_bytes = maker.encode("ascii").ljust(2, b" ")[:2]
    header[0x19:0x1B] = maker_bytes

    game_bytes = game.encode("ascii").ljust(4, b" ")[:4]
    header[0x1B:0x1F] = game_bytes

    header[0x1F] = version

    return bytes(body) + bytes(header)


class TestParseHeader(unittest.TestCase):
    def test_minimum_valid_rom(self):
        rom = _make_rom(MIN_ROM_SIZE, title="TINY ROM",
                        maker="NN", game="TEST", version=2)
        h = parse_header(rom)
        self.assertEqual(h.rom_size, MIN_ROM_SIZE)
        self.assertEqual(h.game_title, "TINY ROM")
        self.assertEqual(h.maker_code, "NN")
        self.assertEqual(h.game_code, "TEST")
        self.assertEqual(h.version, 2)

    def test_sha256_matches_full_rom(self):
        rom = _make_rom(MIN_ROM_SIZE)
        expected = hashlib.sha256(rom).hexdigest()
        h = parse_header(rom)
        self.assertEqual(h.rom_sha256, expected)

    def test_title_truncated_and_stripped(self):
        rom = _make_rom(MIN_ROM_SIZE, title="ABCDEFGHIJKLMNOPQRST_too_long")
        h = parse_header(rom)
        # parser keeps the first 20 chars exactly.
        self.assertEqual(h.game_title, "ABCDEFGHIJKLMNOPQRST")

    def test_undersized_rom_raises(self):
        with self.assertRaises(ValueError):
            parse_header(b"\x00" * (MIN_ROM_SIZE - 1))

    def test_known_sizes(self):
        # 128 KB and 2 MB are both legitimate VB cart sizes.
        self.assertTrue(is_known_size(128 * 1024))
        self.assertTrue(is_known_size(2 * 1024 * 1024))
        # 3.14 KB is not.
        self.assertFalse(is_known_size(3217))


if __name__ == "__main__":
    unittest.main()
