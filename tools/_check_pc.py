"""Quick diagnostic: was a particular PC visited / discovered as a
function on a given cart?"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from recompiler.v810.analysis import (
    RESET_VECTOR,
    RomImage,
    cfg_walk_with_table_resolution,
    discover_functions,
)
from recompiler.v810.decoder import decode_at


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--rom", required=True, type=Path)
    p.add_argument("--pc", type=lambda s: int(s, 0), required=True)
    args = p.parse_args()
    img = RomImage.from_bytes(args.rom.read_bytes())
    walk = cfg_walk_with_table_resolution(img, [RESET_VECTOR])
    fns = discover_functions(img)
    pc = args.pc & 0xFFFFFFFF
    print(f"pc = 0x{pc:08X}")
    print(f"visited by CFG walk: {pc in walk.visited}")
    print(f"in call_targets: {pc in walk.call_targets}")
    print(f"is a discovered function entry: "
          f"{any(f.start_pc == pc for f in fns)}")
    print(f"in any function's range: "
          f"{any(f.start_pc <= pc < f.end_pc for f in fns)}")
    if any(f.start_pc <= pc < f.end_pc for f in fns):
        fn = next(f for f in fns if f.start_pc <= pc < f.end_pc)
        print(f"  containing: 0x{fn.start_pc:08X}..0x{fn.end_pc:08X}")
    # What's at that PC?
    off = img.va_to_offset(pc)
    if off is not None and off + 4 <= img.rom_size:
        ins = decode_at(img.data, off, pc=pc)
        print(f"decode @ pc: {ins.mnemonic} fmt={ins.fmt.name} "
              f"reg1={ins.reg1} reg2={ins.reg2} "
              f"imm16=0x{ins.imm16:04X}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
