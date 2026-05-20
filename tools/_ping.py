"""Smoke probe: send a command to a vbrecomp runtime/oracle TCP debug
server and print the response.

    python tools/_ping.py [--host 127.0.0.1] [--port 4390]
    python tools/_ping.py --port 4391 --cmd frame
    python tools/_ping.py --port 4391 --cmd read_ram \\
        --arg addr=0x07F80120 --arg len=16

Argument values are passed through as strings; the runtime debug
servers accept both integer and 0x-prefixed-hex string literals.
"""
from __future__ import annotations

import argparse
import json
import socket
import sys


def _parse_arg(spec: str) -> tuple[str, object]:
    if "=" not in spec:
        raise argparse.ArgumentTypeError(
            f"--arg expects 'key=value', got {spec!r}")
    k, v = spec.split("=", 1)
    return k.strip(), v


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description="Ping a vbrecomp runtime / oracle.")
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--port", type=int, default=4390)
    p.add_argument("--cmd", default="ping",
                   help="command name to send (default: ping)")
    p.add_argument("--arg", action="append", default=[],
                   type=_parse_arg, metavar="K=V",
                   help="extra request field; repeatable")
    p.add_argument("--timeout", type=float, default=2.0)
    args = p.parse_args(argv)

    req: dict[str, object] = {"cmd": args.cmd, "id": 1}
    for k, v in args.arg:
        req[k] = v

    with socket.create_connection((args.host, args.port), timeout=args.timeout) as s:
        request = json.dumps(req) + "\n"
        s.sendall(request.encode("ascii"))
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    body = data.decode("ascii", errors="replace").strip()
    print(body)
    try:
        parsed = json.loads(body)
        return 0 if parsed.get("ok") else 1
    except json.JSONDecodeError:
        print("WARNING: response is not valid JSON", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
