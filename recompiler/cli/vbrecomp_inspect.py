"""Disassemble a small window of a ROM, no config required.

Useful for spot-checking the decoder by eye against a Ghidra view or
the V810 manual:

    python -m recompiler.cli.vbrecomp_inspect --rom roms/foo.vb \\
        --pc 0xFFFFFFF0 --n 32
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from ..rom.memory_map import rom_offset
from ..v810.decoder import scan


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="vbrecomp-inspect",
        description="Disassemble a window of a VB ROM starting at --pc.",
    )
    p.add_argument("--rom", required=True, type=Path)
    p.add_argument("--pc", required=True, type=lambda s: int(s, 0),
                   help="virtual address to start at (e.g. 0xFFFFFFF0)")
    p.add_argument("--load-base", type=lambda s: int(s, 0), default=0x07000000,
                   help="cart ROM load base (default 0x07000000)")
    p.add_argument("--n", type=int, default=32,
                   help="number of instructions to disassemble")
    return p


def _format_operands(ins) -> str:
    from ..v810.isa import Format
    if ins.fmt is Format.I:
        return f"r{ins.reg2}, r{ins.reg1}"
    if ins.fmt is Format.II:
        return f"#{ins.imm5_s}, r{ins.reg2}"
    if ins.fmt is Format.III:
        if ins.branch_target is not None:
            return f"0x{ins.branch_target:08X}  (disp={ins.disp9_s:+d})"
        return ""
    if ins.fmt is Format.IV:
        return f"0x{ins.branch_target:08X}  (disp={ins.disp26_s:+d})"
    if ins.fmt is Format.V:
        return f"#0x{ins.imm16:04X}, r{ins.reg2}, r{ins.reg1}"
    if ins.fmt is Format.VI:
        return f"{ins.imm16_s:+d}[r{ins.reg1}], r{ins.reg2}"
    if ins.fmt is Format.VII:
        return f"subop={ins.subop:#04x} reg1=r{ins.reg1} reg2=r{ins.reg2}"
    return ""


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    rom = args.rom.read_bytes()

    off = rom_offset(args.pc, len(rom))
    if off is None:
        # Allow inspection outside cart-ROM region too — just compute a
        # plain offset from --load-base.
        off = (args.pc - args.load_base) & (len(rom) - 1 if len(rom) and (len(rom) & (len(rom) - 1)) == 0 else 0xFFFFFFFF)
        if off >= len(rom):
            print(f"--pc 0x{args.pc:08X} does not resolve into the ROM image", file=sys.stderr)
            return 2

    count = 0
    for ins in scan(rom, base_pc=args.load_base, start_offset=off):
        if count >= args.n:
            break
        marker = " " if not ins.is_unknown else "?"
        print(f"{marker} 0x{ins.pc:08X}  {ins.raw.hex():<8}  {ins.mnemonic:<10}  {_format_operands(ins)}")
        count += 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
