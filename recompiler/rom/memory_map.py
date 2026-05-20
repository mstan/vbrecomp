"""Virtual Boy physical memory map.

Per `docs/HARDWARE_NOTES.md` the VB decodes 27 bits of address; the
top 5 bits mirror back into the same 128 MB window. The recompiler
uses this table to:

1. Classify a load/store target address into a region (compile-time
   constant folding when MOVHI/MOVEA-MOVHI pairs nail down the
   effective address).
2. Emit a warning for any access that lands in a reserved region —
   the runtime then routes that to `vb_stub_abort()`.

This module is read-only data plus a `classify()` helper.
"""
from __future__ import annotations

from dataclasses import dataclass
from enum import Enum
from typing import Optional


class Region(Enum):
    VIP = "VIP"          # display controller + VRAM
    VSU = "VSU"          # sound + sound RAM
    MISC = "MISC"        # timer / pad / link / wait
    RESERVED_3 = "RESERVED_3"
    CART_EXP = "CART_EXP"
    WRAM = "WRAM"
    CART_RAM = "CART_RAM"
    CART_ROM = "CART_ROM"


@dataclass(frozen=True)
class MemoryRegion:
    region: Region
    base: int            # physical base (within 27-bit decoded window)
    size: int            # bytes
    is_writable: bool


# Order matters: indexed by bits 26:24 of the physical address.
REGIONS: tuple[MemoryRegion, ...] = (
    MemoryRegion(Region.VIP,        0x00000000, 0x01000000, True),
    MemoryRegion(Region.VSU,        0x01000000, 0x01000000, True),
    MemoryRegion(Region.MISC,       0x02000000, 0x01000000, True),
    MemoryRegion(Region.RESERVED_3, 0x03000000, 0x01000000, False),
    MemoryRegion(Region.CART_EXP,   0x04000000, 0x01000000, True),
    MemoryRegion(Region.WRAM,       0x05000000, 0x01000000, True),
    MemoryRegion(Region.CART_RAM,   0x06000000, 0x01000000, True),
    MemoryRegion(Region.CART_ROM,   0x07000000, 0x01000000, False),
)


PHYS_MASK = 0x07FFFFFF      # 27-bit decoded address space
RESET_VECTOR = 0xFFFFFFF0   # folds to 0x07FFFFF0 under PHYS_MASK


def fold(addr: int) -> int:
    """Apply the 27-bit address-bus mask."""
    return addr & PHYS_MASK


def classify(addr: int) -> MemoryRegion:
    """Return the region a given (virtual) address resolves into."""
    phys = fold(addr)
    # Top 3 bits of the 27-bit decoded address pick the region.
    return REGIONS[(phys >> 24) & 0x7]


def in_region(addr: int, region: Region) -> bool:
    return classify(addr).region is region


def rom_offset(addr: int, rom_size: int) -> Optional[int]:
    """If `addr` lies in cart ROM, return the offset into a ROM image of
    size `rom_size`. Otherwise return None.

    Cart ROM is mirrored within the 16 MB cart-ROM window — for a
    smaller ROM, the bottom `log2(rom_size)` bits select the byte.
    """
    if classify(addr).region is not Region.CART_ROM:
        return None
    if rom_size <= 0:
        return None
    phys = fold(addr) - 0x07000000
    # ROM sizes are powers of two in practice; mask handles mirroring.
    if rom_size & (rom_size - 1) == 0:
        return phys & (rom_size - 1)
    # Non-power-of-two ROM (homebrew oddity) — modulo fallback.
    return phys % rom_size
