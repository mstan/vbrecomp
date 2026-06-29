"""wramhash_compare.py — whole-session WRAM fidelity: recomp vs oracle.

Axis-6 past-the-wall fidelity. Per-instruction lockstep (cpuhook) stops at the
sub-frame VIP-timing wall (~1.4M instrs) where the cart's busy-wait poll loops
read transient VIP status at slightly different sub-frame cycles. That jitter
does not change COMPUTED state — so WRAM should stay identical at every
game-frame boundary, indefinitely.

Both vb-runtime (4390) and vb-beetle (4391) FNV-1a-hash all 64 KiB of WRAM at
each GAME_START VIP event (the game-frame boundary, proven 1:1-aligned by the
vipphase gate) into an always-on from-boot ring. This QUERIES both rings
(Rule 3), aligns them by GAME_START index, and reports the first game frame
whose WRAM fingerprint diverges — extending the fidelity proof through
gameplay, far past the instruction-level wall.

    python tools/wramhash_compare.py --rom roms/marios_tennis.vb
    python tools/wramhash_compare.py --runtime-port 4390 --oracle-port 4391 --frames 40000
"""
from __future__ import annotations

import argparse
import json
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

REGIONS = 64           # 64 x 1 KiB region hashes over the 64 KiB WRAM
REC = 8 + 4 * REGIONS  # bytes: seq(u64) + fnv[64](u32) = 264
PAGE = 4096            # records per query (264 B each → ~2 MB hex/response)


def _send(host: str, port: int, cmd: dict, timeout: float = 6.0) -> dict:
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall((json.dumps(cmd) + "\n").encode("ascii"))
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(1 << 20)
            if not chunk:
                break
            data += chunk
    return json.loads(data.decode("ascii", errors="replace").strip())


def drain(host: str, port: int, want: int, timeout: float) -> list[tuple]:
    """Each element: (seq, (fnv0..fnv63))."""
    out: list[tuple] = []
    start = 0
    while start < want:
        r = _send(host, port,
                  {"cmd": "wram_hash", "id": 1, "start": start, "max": PAGE},
                  timeout)
        if not r.get("ok"):
            raise RuntimeError(f"{port}: {r}")
        raw = bytes.fromhex(r.get("hex", ""))
        got = r.get("returned", 0)
        if got == 0:
            break
        for i in range(0, len(raw), REC):
            vals = struct.unpack_from("<Q" + "I" * REGIONS, raw, i)
            out.append((vals[0], vals[1:]))
        start += got
        if got < PAGE:
            break
    return out


def wait_head(host: str, port: int, target: int, timeout_s: float) -> int:
    deadline = time.time() + timeout_s
    head = 0
    while time.time() < deadline:
        r = _send(host, port, {"cmd": "wram_hash", "id": 1, "start": 0, "max": 1})
        head = r.get("head", 0)
        if head >= target:
            return head
        time.sleep(0.25)
    return head


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--runtime-port", type=int, default=4390)
    p.add_argument("--oracle-port", type=int, default=4391)
    p.add_argument("--frames", type=int, default=40000,
                   help="target number of from-boot game frames to compare")
    p.add_argument("--rom", help="if given, launch both headless first")
    p.add_argument("--runtime-exe",
                   default="build/vbrecomp/runtime/vb-runtime.exe")
    p.add_argument("--oracle-exe",
                   default="build/vbrecomp/runtime/vb-beetle.exe")
    p.add_argument("--timeout", type=float, default=8.0)
    args = p.parse_args(argv)

    procs: list[subprocess.Popen] = []
    try:
        if args.rom:
            for exe, port in ((args.runtime_exe, args.runtime_port),
                              (args.oracle_exe, args.oracle_port)):
                if not Path(exe).exists():
                    print(f"error: missing {exe}", file=sys.stderr)
                    return 2
                procs.append(subprocess.Popen(
                    [str(Path(exe).resolve()), "--rom", args.rom,
                     "--headless", "--port", str(port)],
                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
            time.sleep(1.0)

        rt_head = wait_head(args.host, args.runtime_port, args.frames, 60)
        or_head = wait_head(args.host, args.oracle_port, args.frames, 60)
        n = min(args.frames, rt_head, or_head)
        print(f"comparing [0,{n}) game frames  (runtime head={rt_head} "
              f"oracle head={or_head})")

        rt = drain(args.host, args.runtime_port, n, args.timeout)
        orc = drain(args.host, args.oracle_port, n, args.timeout)
    finally:
        for pr in procs:
            pr.terminate()
        for pr in procs:
            try:
                pr.wait(timeout=5)
            except Exception:
                pr.kill()

    m = min(len(rt), len(orc))
    frames_with_diff = 0
    region_diff_frames = [0] * REGIONS        # per-region: # frames it differs
    total_region_cells = m * REGIONS
    diverged_cells = 0
    for i in range(m):
        rfa = rt[i][1]
        ofa = orc[i][1]
        any_bad = False
        for r in range(REGIONS):
            if rfa[r] != ofa[r]:
                region_diff_frames[r] += 1
                diverged_cells += 1
                any_bad = True
        if any_bad:
            frames_with_diff += 1

    lockstep_regions = [r for r in range(REGIONS) if region_diff_frames[r] == 0]
    diverging_regions = [r for r in range(REGIONS) if region_diff_frames[r] > 0]

    print("=" * 66)
    print("  VB WRAM WHOLE-SESSION FIDELITY — recomp (4390) vs oracle (4391)")
    print("=" * 66)
    print(f"  game frames compared       {m}")
    print(f"  WRAM = 64 x 1KiB regions; total (frame,region) cells = {total_region_cells}")
    print(f"  region-cells in lockstep   {total_region_cells - diverged_cells}"
          f"  ({100.0 * (total_region_cells - diverged_cells) / total_region_cells:.3f}%)")
    print(f"  regions ALWAYS identical   {len(lockstep_regions)} / {REGIONS}")
    print(f"  frames with any diff       {frames_with_diff} / {m}")
    print("-" * 66)
    if not diverging_regions:
        print(f"  VERDICT: WRAM LOCKSTEP — all {REGIONS} regions identical across "
              f"all {m} game frames from boot (past the ~1.4M-instr wall).")
        rc = 0
    else:
        print(f"  VERDICT: divergence confined to {len(diverging_regions)} of "
              f"{REGIONS} regions; the other {len(lockstep_regions)} are in "
              f"perfect lockstep across all {m} frames.")
        for r in diverging_regions:
            lo = r * 1024
            print(f"    region {r:2d} (WRAM 0x{lo:04X}-0x{lo + 1023:04X}): "
                  f"differs in {region_diff_frames[r]}/{m} frames")
        rc = 1
    print("=" * 66)
    return rc


if __name__ == "__main__":
    sys.exit(main())
