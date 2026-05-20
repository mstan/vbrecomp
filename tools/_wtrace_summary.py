"""Summarise the wtrace ring of a running vbrecomp process.

Connects to the TCP debug server, pulls every entry matching the
supplied address/PC window, and groups them by source PC and target
page so the operator can see at a glance where the cart is making
stores and from where.

    python tools/_wtrace_summary.py --port 4390
    python tools/_wtrace_summary.py --port 4390 \\
        --addr-min 0x00000000 --addr-max 0x01000000
    python tools/_wtrace_summary.py --port 4390 --tail 5000

The dump is paginated client-side because the server caps the per-
request limit at 4096 entries.
"""
from __future__ import annotations

import argparse
import collections
import json
import socket
import sys


def send(host: str, port: int, req: dict) -> dict:
    with socket.create_connection((host, port), timeout=5.0) as s:
        s.sendall((json.dumps(req) + "\n").encode("ascii"))
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(65536)
            if not chunk:
                break
            data += chunk
    return json.loads(data.decode("ascii", errors="replace").strip())


def pull(host: str, port: int,
         addr_min: int, addr_max: int,
         pc_min: int, pc_max: int,
         from_seq: int, to_seq: int,
         tail: int,
         page_size: int = 4096) -> list[dict]:
    """Drain the ring window into a list of entries (oldest-first)."""
    out: list[dict] = []
    seq = from_seq
    if tail:
        stats = send(host, port, {"cmd": "wtrace_stats", "id": 1})
        total = stats["total"]
        seq = max(seq, total - tail)
    while True:
        req: dict = {
            "cmd": "wtrace_dump", "id": 1,
            "from_seq": seq, "to_seq": to_seq,
            "addr_min": addr_min, "addr_max": addr_max,
            "pc_min": pc_min, "pc_max": pc_max,
            "limit": page_size,
        }
        resp = send(host, port, req)
        entries = resp.get("entries", [])
        if not entries:
            break
        out.extend(entries)
        last_seq = entries[-1]["seq"]
        if len(entries) < page_size:
            break
        seq = last_seq + 1
        if to_seq and seq >= to_seq:
            break
    return out


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Summarise the wtrace ring.")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=4390)
    p.add_argument("--addr-min", type=lambda s: int(s, 0), default=0)
    p.add_argument("--addr-max", type=lambda s: int(s, 0), default=0,
                   help="exclusive upper bound; 0 = no max")
    p.add_argument("--pc-min", type=lambda s: int(s, 0), default=0)
    p.add_argument("--pc-max", type=lambda s: int(s, 0), default=0)
    p.add_argument("--from-seq", type=int, default=0)
    p.add_argument("--to-seq", type=int, default=0)
    p.add_argument("--tail", type=int, default=0,
                   help="restrict to the last N entries before applying "
                        "filters")
    p.add_argument("--top", type=int, default=15)
    p.add_argument("--show-entries", type=int, default=0,
                   help="print the first N raw matching entries")
    args = p.parse_args(argv)

    entries = pull(args.host, args.port,
                   args.addr_min, args.addr_max,
                   args.pc_min, args.pc_max,
                   args.from_seq, args.to_seq,
                   args.tail)

    if not entries:
        print("(no entries in window)")
        return 0

    print(f"matched {len(entries)} entries "
          f"(seq {entries[0]['seq']}..{entries[-1]['seq']})")

    # Distribution by 4KB page of the target address.
    pages = collections.Counter()
    for e in entries:
        a = int(e["addr"], 16)
        pages[a >> 12] += 1
    print(f"\ntop {args.top} target pages:")
    for pg, n in pages.most_common(args.top):
        print(f"  0x{pg << 12:08X}..0x{(pg + 1) << 12:08X}  {n:>7}")

    # Distribution by source PC.
    pcs = collections.Counter(e["pc"] for e in entries)
    print(f"\ntop {args.top} source PCs:")
    for pc, n in pcs.most_common(args.top):
        print(f"  {pc}  {n:>7}")

    # Distribution by width.
    widths = collections.Counter(e["width"] for e in entries)
    print(f"\nwidth split: {dict(widths)}")

    if args.show_entries:
        print(f"\nfirst {args.show_entries} matching entries:")
        for e in entries[:args.show_entries]:
            print(f"  seq={e['seq']:>7} pc={e['pc']} "
                  f"addr={e['addr']} val={e['value']} w={e['width']}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
