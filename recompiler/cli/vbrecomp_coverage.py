"""Compute V810 opcode coverage over a VB cart — and regenerate
`docs/INSTRUCTION_STATUS.md` from `isa.py`.

Default mode is **CFG-aware** — the scan starts at the reset
trampoline, follows every JAL/Bcond/JR target, and decodes only the
instructions actually reachable. This is the meaningful denominator
("how much of the ISA does this game use"), and the residual unknown
rate is a direct measure of remaining decoder gaps.

Use ``--linear`` to fall back to the linear-scan mode, which decodes
every halfword of the ROM and lumps real instructions in with
post-branch data (jump tables, constant pools, padding). The linear
rate is useful for catching regressions in the decoder against pure
"how many things look like instructions" expectations; CFG-aware is
the rate that matters for L1 oracle parity.

Use ``--regen-status`` (no --rom required) to rewrite
`docs/INSTRUCTION_STATUS.md` mechanically from the live `isa.py`
tables, cross-checked against Beetle VB's op table.

    python -m recompiler.cli.vbrecomp_coverage --rom roms/foo.vb
    python -m recompiler.cli.vbrecomp_coverage --rom roms/foo.vb --linear
    python -m recompiler.cli.vbrecomp_coverage --regen-status
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from ..rom.header import parse_header
from ..v810.analysis import (
    RomImage,
    cfg_walk_with_table_resolution,
    discover_functions,
    trace_reset_trampoline,
)
from ..v810.decoder import scan
from ..v810.status import coverage_of


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="vbrecomp-coverage",
        description="V810 opcode coverage histogram over a VB ROM, "
                    "or regenerator for docs/INSTRUCTION_STATUS.md.",
    )
    p.add_argument("--rom", type=Path,
                   help="cart ROM to analyse (required unless --regen-status)")
    p.add_argument("--load-base", type=lambda s: int(s, 0), default=0x07000000)
    p.add_argument("--start-offset", type=lambda s: int(s, 0), default=0,
                   help="byte offset into the ROM (linear mode only)")
    p.add_argument("--show-unknowns", type=int, default=32)
    p.add_argument("--show-top", type=int, default=40)
    p.add_argument("--linear", action="store_true",
                   help="linear scan instead of CFG-aware walk (slower, "
                        "noisier — only useful to compare against the "
                        "CFG-aware rate)")
    p.add_argument("--regen-status", action="store_true",
                   help="rewrite docs/INSTRUCTION_STATUS.md from isa.py + "
                        "Beetle VB's op table; ignores --rom")
    return p


def _run_linear(rom: bytes, args: argparse.Namespace) -> int:
    insns = list(scan(rom, base_pc=args.load_base,
                      start_offset=args.start_offset))
    rep = coverage_of(insns, unknown_sample_limit=args.show_unknowns)
    print(f"[linear scan]  Decoded {rep.total_insns} instructions; "
          f"{rep.unknown_insns} unknown "
          f"({rep.unknown_rate * 100:.2f}%).")
    print()
    print(f"Top {args.show_top} mnemonics:")
    for mnem, count in rep.top(args.show_top):
        print(f"  {mnem:<12} {count}")
    if rep.unknown_examples:
        print()
        print(f"First {len(rep.unknown_examples)} unknown encodings:")
        for pc, hex_raw in rep.unknown_examples:
            print(f"  pc=0x{pc:08X}  raw={hex_raw}")
    return 0 if rep.unknown_insns == 0 else 1


def _run_cfg(rom_bytes: bytes, args: argparse.Namespace) -> int:
    img = RomImage.from_bytes(rom_bytes)
    entry = trace_reset_trampoline(img)
    if entry is None:
        print("[CFG] trampoline tracer could not resolve the cart entry "
              "PC. The cart's reset thunk at 0xFFFFFFF0 uses an encoding "
              "outside the tracer's whitelist (see analysis.py). Falling "
              "back to --linear mode is the diagnostic move.")
        return 2

    walk = cfg_walk_with_table_resolution(img, [entry])
    fns = discover_functions(img)

    # Decode every visited PC into a list of DecodedInstruction so we
    # can re-use the existing coverage_of() histogram code.
    insns = [img.decode_at_va(pc) for pc in sorted(walk.visited.keys())]
    insns = [i for i in insns if i is not None]
    rep = coverage_of(insns, unknown_sample_limit=args.show_unknowns)

    print(f"[CFG-aware]")
    print(f"  Entry PC:           0x{entry:08X}")
    print(f"  Functions found:    {len(fns)}")
    print(f"  Visited instrs:     {rep.total_insns}")
    print(f"  Unknown instrs:     {rep.unknown_insns} "
          f"({rep.unknown_rate * 100:.2f}%)")
    print(f"  Call sites (JAL):   {len(walk.call_targets)}")
    print(f"  Indirect jumps:     {len(walk.indirect_jumps)}")
    print(f"  Off-cart branches:  {len(walk.off_cart_pcs)}")
    print()
    print(f"Top {args.show_top} mnemonics (reachable code only):")
    for mnem, count in rep.top(args.show_top):
        print(f"  {mnem:<12} {count}")

    if rep.unknown_examples:
        print()
        print(f"First {len(rep.unknown_examples)} unknown encodings "
              f"in reachable code:")
        for pc, hex_raw in rep.unknown_examples:
            print(f"  pc=0x{pc:08X}  raw={hex_raw}")

    return 0 if rep.unknown_insns == 0 else 1


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    if args.regen_status:
        from .vbrecomp_status import regen_instruction_status
        return regen_instruction_status()

    if args.rom is None:
        print("error: --rom is required unless --regen-status is given",
              file=sys.stderr)
        return 2
    rom_bytes = args.rom.read_bytes()
    header = parse_header(rom_bytes)

    # Some commercial cart titles include bytes that don't survive the
    # console's default code page (e.g. cp1252 on Windows). Round-trip
    # through ASCII with replacement so the report renders cleanly
    # regardless of locale.
    safe_title = header.game_title.encode("ascii", "replace").decode("ascii")
    print(f"ROM:     {args.rom}  ({header.rom_size} bytes)")
    print(f"SHA-256: {header.rom_sha256}")
    print(f"Title:   {safe_title!r}  "
          f"(maker={header.maker_code!r}  game={header.game_code!r})")
    print()

    if args.linear:
        return _run_linear(rom_bytes, args)
    return _run_cfg(rom_bytes, args)


if __name__ == "__main__":
    sys.exit(main())
