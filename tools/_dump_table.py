"""Dump a contiguous 32-bit table from a ROM."""
from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--rom", required=True, type=Path)
    p.add_argument("--va", type=lambda s: int(s, 0), required=True)
    p.add_argument("--n", type=int, default=16)
    args = p.parse_args()
    data = args.rom.read_bytes()
    phys = args.va & 0x07FFFFFF
    # Bank 7 base = 0x07000000
    off = (phys - 0x07000000) & (len(data) - 1)
    print(f"table at va=0x{args.va:08X}, rom_off=0x{off:06X}:")
    for i in range(args.n):
        w = struct.unpack_from("<I", data, off + i * 4)[0]
        print(f"  [{i:2d}] = 0x{w:08X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
