"""Virtual Boy ROM header parser.

The VB header occupies the last 544 bytes (0x220) of a cart ROM.
Layout (offsets relative to header start):

    0x000  Game Title              20 bytes ASCII (space-padded)
    0x014  Reserved                 5 bytes
    0x019  Maker Code               2 bytes ASCII
    0x01B  Game Code                4 bytes ASCII
    0x01F  Version                  1 byte

The bulk of the 544-byte region is reserved / vector table copy; the
fields above are the ones present in every cart and worth validating.

Sources cross-referenced when populating this file:
- Virtual Boy Programmer's Manual
- Planet Virtual Boy Sacred Tech Scroll
- Mednafen Beetle VB source (cart header detection)
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass
from pathlib import Path
from typing import Union


HEADER_SIZE = 0x220   # 544 bytes
MIN_ROM_SIZE = 0x10000     # 64 KB — smallest plausible cart
# Common cart sizes in bits (1 Mbit = 128 KB, the smallest commercial cart).
KNOWN_ROM_SIZES = (
    1 * 1024 * 1024 // 8,     # 1 Mbit = 128 KB (homebrew floor)
    2 * 1024 * 1024 // 8,     # 2 Mbit = 256 KB
    4 * 1024 * 1024 // 8,     # 4 Mbit = 512 KB
    8 * 1024 * 1024 // 8,     # 8 Mbit = 1 MB
    16 * 1024 * 1024 // 8,    # 16 Mbit = 2 MB (Wario Land)
)


@dataclass(frozen=True)
class VBHeader:
    """Parsed Virtual Boy cartridge header."""

    rom_size: int             # bytes
    rom_sha256: str           # hex
    game_title: str           # stripped
    maker_code: str
    game_code: str
    version: int

    @property
    def is_power_of_two(self) -> bool:
        return self.rom_size > 0 and (self.rom_size & (self.rom_size - 1)) == 0


def load_rom(path: Union[str, Path]) -> bytes:
    """Read a ROM file from disk. Returns the raw bytes."""
    return Path(path).read_bytes()


def parse_header(rom: bytes) -> VBHeader:
    """Parse the header at the end of `rom` and validate basic invariants.

    Raises ValueError on truncated or grossly malformed input.
    """
    if len(rom) < MIN_ROM_SIZE:
        raise ValueError(
            f"ROM is too small ({len(rom)} bytes) — Virtual Boy carts are "
            f">= {MIN_ROM_SIZE} bytes; this is unlikely to be a VB ROM"
        )
    if len(rom) < HEADER_SIZE:
        raise ValueError(
            f"ROM ({len(rom)} bytes) is smaller than the VB header "
            f"({HEADER_SIZE} bytes)"
        )

    base = len(rom) - HEADER_SIZE

    title_bytes = rom[base:base + 0x14]
    # ASCII; space-padded to 20 chars. Replace nulls and strip.
    title = title_bytes.replace(b"\x00", b" ").decode("ascii", errors="replace").rstrip()

    maker = rom[base + 0x19:base + 0x1B].decode("ascii", errors="replace")
    game_code = rom[base + 0x1B:base + 0x1F].decode("ascii", errors="replace")
    version = rom[base + 0x1F]

    sha = hashlib.sha256(rom).hexdigest()

    return VBHeader(
        rom_size=len(rom),
        rom_sha256=sha,
        game_title=title,
        maker_code=maker,
        game_code=game_code,
        version=version,
    )


def is_known_size(rom_size: int) -> bool:
    """True if `rom_size` matches a known commercial cart size in bytes."""
    return rom_size in KNOWN_ROM_SIZES
