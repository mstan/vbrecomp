"""Side-by-side WRAM diff between vb-runtime (4390) and vb-beetle (4391).

Pulls the same WRAM byte range from both processes and reports the
first N differences, with each pair printed as a hex line so the
operator can spot game-state mismatches quickly.

    python tools/_wram_diff.py
    python tools/_wram_diff.py --base 0x05000000 --len 0x10000
    python tools/_wram_diff.py --limit 32
"""
from __future__ import annotations

import argparse
import binascii
import json
import socket
import sys


def fetch(port: int, addr: int, length: int) -> bytes:
    """read_ram returns at most 4096 bytes per call; paginate."""
    out = bytearray()
    remaining = length
    cur = addr
    while remaining > 0:
        chunk = min(remaining, 4096)
        with socket.create_connection(("127.0.0.1", port), timeout=5.0) as s:
            req = {"cmd": "read_ram", "addr": cur, "len": chunk, "id": 1}
            s.sendall((json.dumps(req) + "\n").encode("ascii"))
            data = b""
            while not data.endswith(b"\n"):
                buf = s.recv(65536)
                if not buf:
                    break
                data += buf
        body = json.loads(data.decode("ascii", errors="replace").strip())
        if not body.get("ok"):
            raise RuntimeError(f"read_ram@{port} failed: {body}")
        hex_blob = body["hex"]
        out.extend(binascii.unhexlify(hex_blob))
        cur += chunk
        remaining -= chunk
    return bytes(out)


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Diff WRAM across both processes.")
    p.add_argument("--base", type=lambda s: int(s, 0), default=0x05000000)
    p.add_argument("--len",  type=lambda s: int(s, 0), default=0x10000,
                   dest="length")
    p.add_argument("--limit", type=int, default=64,
                   help="max #difference rows to print")
    p.add_argument("--row", type=int, default=16,
                   help="bytes per diff row")
    args = p.parse_args(argv)

    a = fetch(4390, args.base, args.length)
    b = fetch(4391, args.base, args.length)
    if len(a) != len(b):
        print(f"length mismatch: runtime={len(a)} beetle={len(b)}",
              file=sys.stderr)
        return 2

    diff_count = 0
    row = args.row
    rows_printed = 0
    print(f"comparing WRAM 0x{args.base:08X}..0x{args.base + args.length:08X} "
          f"({args.length} bytes)")
    for off in range(0, len(a), row):
        ra = a[off:off + row]
        rb = b[off:off + row]
        if ra == rb:
            continue
        diff_count += sum(1 for x, y in zip(ra, rb) if x != y)
        if rows_printed < args.limit:
            mark = "".join("." if x == y else "X" for x, y in zip(ra, rb))
            print(f"  0x{args.base + off:08X}  "
                  f"R={ra.hex(' ')}  "
                  f"B={rb.hex(' ')}  {mark}")
            rows_printed += 1

    print(f"\ntotal differing bytes: {diff_count} / {len(a)} "
          f"({100.0 * diff_count / len(a):.2f}%)")
    return 0 if diff_count == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
