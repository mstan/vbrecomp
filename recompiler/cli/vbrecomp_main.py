"""Primary CLI: load a game config, decode, report opcode coverage.

Phase 1 scope:
  - load TOML
  - parse the VB header
  - linearly decode the ROM from `entry_pc`
  - print opcode coverage + first-unknown samples
  - DO NOT emit C (that's Phase 3)
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

from ..config.game_toml import load_game_config
from ..rom.header import parse_header
from ..rom.memory_map import rom_offset
from ..v810.decoder import scan
from ..v810.status import coverage_of


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="vbrecomp",
        description="Static V810-to-C recompiler for the Nintendo Virtual Boy "
                    "(Phase 1: decode + coverage only; no codegen).",
    )
    p.add_argument("--config", required=True, type=Path,
                   help="path to per-game TOML config (see games/*.toml.example)")
    p.add_argument("--max-insns", type=int, default=0,
                   help="if > 0, stop after this many decoded instructions "
                        "(handy for smoke runs on large ROMs)")
    p.add_argument("--show-unknowns", type=int, default=16,
                   help="how many distinct unknown-opcode samples to print")
    return p


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)

    cfg = load_game_config(args.config)
    if not cfg.rom_path.is_file():
        print(f"ERROR: ROM file not found at {cfg.rom_path}", file=sys.stderr)
        print("(Place the ROM at that path, or edit the [program].rom field "
              "in the TOML.)", file=sys.stderr)
        return 2

    rom = cfg.rom_path.read_bytes()
    header = parse_header(rom)

    print(f"Game:        {cfg.name}")
    print(f"Config:      {cfg.config_path}")
    print(f"ROM:         {cfg.rom_path}  ({header.rom_size} bytes)")
    print(f"SHA-256:     {header.rom_sha256}")
    if cfg.rom_sha256 and cfg.rom_sha256 != header.rom_sha256:
        print(f"WARNING: configured rom_sha256 does not match file:")
        print(f"  config: {cfg.rom_sha256}")
        print(f"  file:   {header.rom_sha256}")
    print(f"Title:       {header.game_title!r}")
    print(f"Maker/Game:  {header.maker_code!r} / {header.game_code!r}  v{header.version}")
    print(f"Entry PC:    0x{cfg.entry_pc:08X}  (load_base 0x{cfg.load_base:08X})")
    print()

    # Resolve the entry PC into a ROM offset and scan forward.
    off = rom_offset(cfg.entry_pc, len(rom))
    if off is None:
        print(f"ERROR: entry_pc 0x{cfg.entry_pc:08X} does not resolve to cart ROM "
              "under the VB physical map. Check [program].entry_pc.", file=sys.stderr)
        return 2

    print(f"Scanning from offset 0x{off:06X} (linear; control flow ignored)...")
    insns = list(scan(rom, base_pc=cfg.load_base, start_offset=off))
    if args.max_insns > 0:
        insns = insns[:args.max_insns]

    rep = coverage_of(insns, unknown_sample_limit=args.show_unknowns)

    print()
    print(f"Decoded {rep.total_insns} instructions; {rep.unknown_insns} unknown "
          f"({rep.unknown_rate * 100:.2f}%).")
    print()
    print("Top 20 mnemonics:")
    for mnem, count in rep.top(20):
        print(f"  {mnem:<12} {count}")

    if rep.unknown_examples:
        print()
        print(f"First {len(rep.unknown_examples)} unknown encodings:")
        for pc, hex_raw in rep.unknown_examples:
            print(f"  pc=0x{pc:08X}  raw={hex_raw}")

    # Non-zero exit if anything decoded as unknown — a hard signal for CI.
    return 0 if rep.unknown_insns == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
