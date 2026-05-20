"""Diagnose 'unknown' decoder hits.

For a given ROM, walk the linear decode and bucket every unknown by
its 6-bit primary opcode (or FPP/BSU sub-op). Print:
  - histogram of unknown buckets
  - for each bucket, up to 8 sample (pc, raw, surrounding bytes)
  - run-length info: does each unknown sit alone amid valid code,
    or does it appear in long contiguous runs (likely data)?

If the unknowns cluster in long runs at predictable offsets, that
points at data sections. If they're sprinkled inside valid-looking
code with surrounding context that looks like correct instructions,
that points at missing opcodes in our isa.py.
"""
from __future__ import annotations

import argparse
import sys
from collections import Counter, defaultdict
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))

from recompiler.v810.decoder import decode_at, scan
from recompiler.v810.isa import Format


def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--rom", required=True, type=Path)
    p.add_argument("--load-base", type=lambda s: int(s, 0), default=0x07000000)
    p.add_argument("--samples", type=int, default=8)
    args = p.parse_args()

    rom = args.rom.read_bytes()
    insns = list(scan(rom, base_pc=args.load_base))

    by_bucket: Counter[str] = Counter()
    samples: dict[str, list[tuple[int, str]]] = defaultdict(list)
    runs: list[tuple[int, int, str]] = []     # (start_pc, length, bucket)
    cur_run: list[int] = []
    cur_bucket: str | None = None

    def flush_run() -> None:
        nonlocal cur_run, cur_bucket
        if cur_run and cur_bucket:
            runs.append((cur_run[0], len(cur_run), cur_bucket))
        cur_run = []
        cur_bucket = None

    for ins in insns:
        if not ins.is_unknown:
            flush_run()
            continue
        bucket = ins.mnemonic
        by_bucket[bucket] += 1
        if len(samples[bucket]) < args.samples:
            samples[bucket].append((ins.pc, ins.raw.hex()))
        if cur_bucket == bucket:
            cur_run.append(ins.pc)
        else:
            flush_run()
            cur_bucket = bucket
            cur_run = [ins.pc]
    flush_run()

    print(f"ROM: {args.rom}  ({len(rom)} bytes)")
    print(f"Decoded: {len(insns)}; unknowns: {sum(by_bucket.values())}")
    print()
    print("Unknowns by bucket:")
    for bucket, n in by_bucket.most_common():
        print(f"  {bucket:<24} {n}")
    print()

    print("Run-length analysis (long runs == likely data; short isolated == likely missing opcode):")
    runs.sort(key=lambda t: -t[1])
    for start, length, bucket in runs[:15]:
        print(f"  bucket={bucket:<24} start=0x{start:08X}  length={length}")
    print()

    isolated_in_code = []
    # An 'isolated' unknown is one with valid instructions on both sides.
    for i, ins in enumerate(insns):
        if not ins.is_unknown:
            continue
        prev_ok = i > 0 and not insns[i - 1].is_unknown
        next_ok = i + 1 < len(insns) and not insns[i + 1].is_unknown
        if prev_ok and next_ok:
            isolated_in_code.append(ins)
    print(f"Isolated unknowns (surrounded by valid insns on both sides): {len(isolated_in_code)}")
    for ins in isolated_in_code[:20]:
        ctx_lo = max(0, insns.index(ins) - 1)
        prev = insns[ctx_lo]
        nxt = insns[insns.index(ins) + 1] if insns.index(ins) + 1 < len(insns) else None
        nxt_mn = nxt.mnemonic if nxt else "<eof>"
        print(f"  pc=0x{ins.pc:08X} raw={ins.raw.hex():<8} {ins.mnemonic:<24}  "
              f"prev={prev.mnemonic:<10}  next={nxt_mn}")

    print()
    print("Bucket samples (first N for each bucket):")
    for bucket in by_bucket:
        print(f"  {bucket}:")
        for pc, raw in samples[bucket][:args.samples]:
            print(f"    pc=0x{pc:08X} raw={raw}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
