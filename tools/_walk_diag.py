"""Quick diagnostic: dump CFG walk indirect jumps + visited PCs for a cart."""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Allow running as a standalone script.
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from recompiler.v810.analysis import (
    RomImage,
    cfg_walk_with_table_resolution,
    discover_functions,
    trace_reset_trampoline,
)


def main() -> int:
    p = argparse.ArgumentParser(prog="_walk_diag")
    p.add_argument("--rom", required=True, type=Path)
    p.add_argument("--show-visited", action="store_true",
                   help="dump every visited PC and decoded mnemonic")
    p.add_argument("--around-indirect", type=int, default=8,
                   help="show N instructions of context before each indirect jump")
    args = p.parse_args()

    img = RomImage.from_bytes(args.rom.read_bytes())
    entry = trace_reset_trampoline(img)
    print(f"entry: 0x{entry:08X}" if entry else "entry: <unresolved>")
    walk = cfg_walk_with_table_resolution(img, [entry] if entry else [])
    fns = discover_functions(img)
    print(f"functions: {len(fns)}")
    print(f"visited: {len(walk.visited)}")
    print(f"call_targets: {len(walk.call_targets)}")
    print(f"indirect_jumps: {len(walk.indirect_jumps)}")
    print(f"off_cart_pcs: {len(walk.off_cart_pcs)}")
    print(f"unknown_pcs: {len(walk.unknown_pcs)}")

    if walk.indirect_jumps:
        print()
        print("Indirect jumps with context:")
        for jump_pc, reg in walk.indirect_jumps.items():
            print(f"\n  JMP r{reg} at 0x{jump_pc:08X}")
            # Walk backwards through visited PCs to print a few prior
            # instructions (best effort — visited isn't a linear stream).
            sorted_pcs = sorted(p for p in walk.visited if p <= jump_pc)
            ctx = sorted_pcs[-args.around_indirect:]
            for pc in ctx:
                ins = img.decode_at_va(pc)
                if ins is None:
                    continue
                marker = " <-- INDIRECT" if pc == jump_pc else ""
                print(f"    0x{pc:08X}  {ins.mnemonic:8} "
                      f"r1={ins.reg1:2d} r2={ins.reg2:2d} "
                      f"imm16=0x{ins.imm16:04X}{marker}")

    if args.show_visited:
        print()
        print("All visited PCs:")
        for pc in sorted(walk.visited):
            ins = img.decode_at_va(pc)
            if ins is None:
                continue
            print(f"  0x{pc:08X}  {ins.mnemonic}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
