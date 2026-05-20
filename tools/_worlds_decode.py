"""Decode the WORLDS table from vb-runtime or vb-beetle (with patch).

Reads 1024 bytes at 0x0003D800 over read_ram, prints any non-zero
world entry decoded as VB WORLD HEAD bitfield.

    python tools/_worlds_decode.py --port 4390
"""
from __future__ import annotations

import argparse
import binascii
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


def decode_head(h: int) -> str:
    lon = (h >> 15) & 1
    ron = (h >> 14) & 1
    bgm = (h >> 12) & 3
    scx = (h >> 10) & 3
    scy = (h >> 8)  & 3
    over = (h >> 7) & 1
    end = (h >> 6) & 1
    bgmap_base = h & 0xF
    bgm_name = {0: "Normal", 1: "HBias", 2: "Affine", 3: "OBJ"}[bgm]
    flags = []
    if lon: flags.append("LON")
    if ron: flags.append("RON")
    if over: flags.append("OVER")
    if end: flags.append("END")
    return (f"BGM={bgm_name} SCX={1 << scx} SCY={1 << scy} "
            f"base={bgmap_base} [{','.join(flags) or '-'}]")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--port", type=int, default=4390)
    p.add_argument("--addr", type=lambda s: int(s, 0), default=0x0003D800)
    p.add_argument("--len",  type=lambda s: int(s, 0), default=1024,
                   dest="length")
    args = p.parse_args(argv)

    resp = send(args.port, {"cmd": "read_ram", "id": 1,
                            "addr": args.addr, "len": args.length})
    if not resp.get("ok"):
        print(resp, file=sys.stderr)
        return 1
    data = binascii.unhexlify(resp["hex"])

    print(f"WORLDS @ 0x{args.addr:08X}  ({args.length} bytes, "
          f"{args.length // 32} entries)")
    any_nonzero = False
    for w in range(args.length // 32):
        off = w * 32
        entry = data[off:off + 32]
        if entry == b"\x00" * 32:
            continue
        any_nonzero = True
        head = entry[0] | (entry[1] << 8)
        gx = int.from_bytes(entry[2:4], "little", signed=True)
        gp = int.from_bytes(entry[4:6], "little", signed=True)
        gy = int.from_bytes(entry[6:8], "little", signed=True)
        mx = int.from_bytes(entry[8:10], "little", signed=True)
        mp = int.from_bytes(entry[10:12], "little", signed=True)
        my = int.from_bytes(entry[12:14], "little", signed=True)
        wsz = int.from_bytes(entry[14:16], "little", signed=False)
        hsz = int.from_bytes(entry[16:18], "little", signed=False)
        print(f"  WORLD[{w:2d}] @ +0x{off:03X}: HEAD=0x{head:04X} "
              f"({decode_head(head)})")
        print(f"    GX={gx} GP={gp} GY={gy} | MX={mx} MP={mp} MY={my} "
              f"| W={wsz} H={hsz}")
        if w < 31:  # not the END marker; show full hex too
            print(f"    raw: {entry.hex(' ')}")
    if not any_nonzero:
        print("  (all entries zero)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
