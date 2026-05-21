"""Recompile a Virtual Boy cart to C — Phase 3 codegen driver.

    python -m recompiler.cli.vbrecomp_codegen --rom roms/marios_tennis.vb
    python -m recompiler.cli.vbrecomp_codegen --rom roms/marios_tennis.vb \\
        --module marios_tennis --out generated/ --limit 50 \\
        --seeds-toml marios_tennis.toml

The driver discovers functions via the CFG-aware walker, emits the
three generated artifacts (`<module>_full.c`, `<module>_dispatch.c`,
`<module>.h`), and writes them to `--out`. `--limit N` truncates the
function set to the first N (BFS-from-entry order); functions outside
the limit will fatal-abort if reached at runtime, surfacing the gap
per CLAUDE.md §0.

`--seeds-toml PATH` reads a per-cart toml and treats its
`[functions].seeds = ["0xFFF8...", ...]` list as extra function-
entry PCs handed to the discovery walker — the escape hatch for
function-pointer tables the static walker can't follow.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import List

import tomli

from ..rom.header import parse_header
from ..v810.analysis import RomImage
from ..v810.emitter import recompile_rom


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="vbrecomp-codegen",
        description="Recompile a VB cart's reachable code to C.",
    )
    p.add_argument("--rom", required=True, type=Path)
    p.add_argument("--out", type=Path, default=Path("generated"),
                   help="output directory (default: generated/)")
    p.add_argument("--module", type=str, default=None,
                   help="module name; defaults to the rom filename stem "
                        "(e.g. roms/marios_tennis.vb -> 'marios_tennis')")
    p.add_argument("--limit", type=int, default=None,
                   help="recompile only the first N functions "
                        "(BFS-from-entry order)")
    p.add_argument("--seeds-toml", type=Path, default=None,
                   help="per-cart toml file with explicit "
                        "[functions].seeds = [...] PCs to seed CFG "
                        "discovery alongside RESET_VECTOR + handlers")
    return p


def _load_extra_seeds(path: Path) -> List[int]:
    """Parse [functions].seeds from a per-cart toml.

    Accepts entries as 0x-prefixed hex strings or plain integers.
    Bare hex without 0x is rejected to keep the format unambiguous.
    """
    raw = tomli.loads(path.read_text(encoding="utf-8"))
    funcs = raw.get("functions", {})
    seeds_raw = funcs.get("seeds", [])
    out: List[int] = []
    for s in seeds_raw:
        if isinstance(s, int):
            out.append(s)
            continue
        if not isinstance(s, str):
            raise SystemExit(
                f"--seeds-toml {path}: [functions].seeds entries must be "
                f"int or '0x...'-prefixed hex string; got {s!r}"
            )
        s_clean = s.strip()
        if not (s_clean.startswith("0x") or s_clean.startswith("0X")):
            raise SystemExit(
                f"--seeds-toml {path}: seed {s!r} must start with '0x'"
            )
        out.append(int(s_clean, 16))
    return out


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    rom_bytes = args.rom.read_bytes()
    header = parse_header(rom_bytes)
    safe_title = header.game_title.encode("ascii", "replace").decode("ascii")
    module = args.module or args.rom.stem

    print(f"ROM:     {args.rom}  ({header.rom_size} bytes)")
    print(f"SHA-256: {header.rom_sha256}")
    print(f"Title:   {safe_title!r}  (module={module!r})")

    extra_seeds: List[int] = []
    if args.seeds_toml is not None:
        extra_seeds = _load_extra_seeds(args.seeds_toml)
        print(f"Seeds:   {len(extra_seeds)} from {args.seeds_toml}")
    print()

    img = RomImage.from_bytes(rom_bytes)
    result = recompile_rom(img, module_name=module,
                           function_limit=args.limit,
                           extra_seeds=extra_seeds or None)

    if not result.files:
        print("error: codegen produced no output (function discovery "
              "returned empty — see analysis.trace_reset_trampoline "
              "for hints)", file=sys.stderr)
        return 2

    args.out.mkdir(parents=True, exist_ok=True)
    # Each `relpath` is `generated/<file>`; we strip the "generated/"
    # prefix here so the user can drop the output anywhere.
    for relpath, content in result.files.items():
        name = relpath.split("/", 1)[1] if "/" in relpath else relpath
        target = args.out / name
        target.write_text(content, encoding="utf-8")
        print(f"  wrote {target} ({len(content)} bytes)")

    print()
    print(f"Functions: {result.function_count}")
    print(f"Instructions: {result.instruction_count}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
