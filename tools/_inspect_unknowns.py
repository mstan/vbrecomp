"""Quick diagnostic — list the encoding of every unknown PC the walker
hit on a given ROM. Used to triage residual decoder gaps."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from recompiler.v810.analysis import (
    RomImage,
    cfg_walk_with_table_resolution,
    trace_reset_trampoline,
)
from recompiler.v810.decoder import decode_at


def main() -> int:
    p = argparse.ArgumentParser(prog="_inspect_unknowns")
    p.add_argument("--rom", required=True, type=Path)
    args = p.parse_args()

    img = RomImage.from_bytes(args.rom.read_bytes())
    entry = trace_reset_trampoline(img)
    walk = cfg_walk_with_table_resolution(img, [entry] if entry else [])
    if not walk.unknown_pcs:
        print(f"no unknowns in {args.rom.name}")
        return 0
    print(f"{len(walk.unknown_pcs)} unknown PCs in {args.rom.name}:")
    for pc in sorted(walk.unknown_pcs):
        off = img.va_to_offset(pc)
        if off is None:
            continue
        ins = decode_at(img.data, off, pc=pc)
        print(f"  pc=0x{pc:08X}  raw={ins.raw.hex()}  fmt={ins.fmt.name}  "
              f"primary=0x{ins.opcode6:02X}  mnemonic={ins.mnemonic!r}  "
              f"notes={ins.notes!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
