"""Walk the fntrace ring backward to find the cart's last non-ISR
function entries.

Steady-state ISR cycle on Mario's Tennis runs through a small set of
PCs (0xFFFFFE40 → 0xFFF800AA → 0xFFF80292 → 0xFFF805B8 → ...). The
moment of interest is *before* the first IRQ-vector entry — that's
when boot-time cart execution finished and unwound to the sentinel.

Usage:
    python tools/_fntrace_walk.py --port 4390
    python tools/_fntrace_walk.py --port 4390 --pre-isr 60

Prints the last K entries before the FIRST IRQ-vector entry (default
40), so you can see what the cart's boot code was doing right before
it stopped progressing.
"""
from __future__ import annotations

import argparse
import json
import socket
import sys


def send(port: int, req: dict) -> dict:
    with socket.create_connection(("127.0.0.1", port), timeout=5.0) as s:
        s.sendall((json.dumps(req) + "\n").encode("ascii"))
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    return json.loads(data.decode("ascii", errors="replace").strip())


def pull_range(port: int, from_seq: int, count: int,
               page: int = 4096) -> list[dict]:
    out: list[dict] = []
    seq = from_seq
    end = from_seq + count
    while seq < end:
        take = min(page, end - seq)
        resp = send(port, {"cmd": "fntrace_dump", "id": 1,
                           "from_seq": seq, "limit": take})
        es = resp.get("entries", [])
        if not es:
            break
        out.extend(es)
        seq = es[-1]["seq"] + 1
        if len(es) < take:
            break
    return out


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=4390)
    p.add_argument("--isr-pc", type=lambda s: int(s, 0), default=0xFFFFFE40,
                   help="PC that marks the IRQ vector entry")
    p.add_argument("--pre-isr", type=int, default=40,
                   help="how many entries to show before first ISR entry")
    p.add_argument("--isr-context", type=int, default=10,
                   help="how many entries of ISR-pattern to show after the "
                        "first ISR entry, for context")
    args = p.parse_args(argv)

    stats = send(args.port, {"cmd": "fntrace_stats", "id": 1})
    total = stats["total"]
    cap = stats["capacity"]
    oldest = max(0, total - cap)
    print(f"fntrace: total={total} oldest_seq={oldest} newest_seq={total - 1}")

    # Search for first ISR entry, scanning from oldest.
    page = 4096
    first_isr_seq: int | None = None
    seq = oldest
    while seq < total and first_isr_seq is None:
        es = pull_range(args.port, seq, page, page=page)
        if not es:
            break
        for e in es:
            if int(e["pc"], 16) == args.isr_pc:
                first_isr_seq = e["seq"]
                break
        seq = es[-1]["seq"] + 1

    if first_isr_seq is None:
        print(f"no entry with pc=0x{args.isr_pc:08X} found", file=sys.stderr)
        return 2
    print(f"first ISR entry at seq {first_isr_seq}")

    # Pull window around it.
    lo = max(oldest, first_isr_seq - args.pre_isr)
    hi = min(total, first_isr_seq + args.isr_context + 1)
    window = pull_range(args.port, lo, hi - lo, page=4096)
    print(f"\n--- last {args.pre_isr} entries BEFORE first ISR ---")
    for e in window:
        if e["seq"] > first_isr_seq + args.isr_context:
            break
        marker = "  <-- first ISR" if e["seq"] == first_isr_seq else ""
        marker = (" (ISR)" if e["seq"] > first_isr_seq else marker)
        print(f"  seq={e['seq']:>6} cycle={e['cycle']:>12} "
              f"pc={e['pc']} lp={e['lp']}{marker}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
