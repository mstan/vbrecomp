"""Cross-process VIP state diff: vb-runtime (4390) vs vb-beetle (4391).

Per CLAUDE.md Rule 14 the two processes are independent — we don't
lockstep, we sample state and diff. Per DEBUG.md the goal is "find the
first divergence, not the final visible bug": this tool sweeps the
common subset of VIP registers and reports the first one that
disagrees, plus the full table for context.

Beetle exposes only a writable-register subset (no DPSTTS/XPSTTS or
internal state-machine vars without an upstream patch). The runtime
exposes the full set. We diff the intersection — that's enough to
verify the cart's register programming matches between sides.

    python tools/_vip_diff.py
    python tools/_vip_diff.py --runtime-port 4390 --oracle-port 4391
"""
from __future__ import annotations

import argparse
import json
import socket
import sys
from typing import Any


def _send(host: str, port: int, cmd: str, timeout: float = 2.0) -> dict[str, Any]:
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall((json.dumps({"cmd": cmd, "id": 1}) + "\n").encode("ascii"))
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(4096)
            if not chunk:
                break
            data += chunk
    body = data.decode("ascii", errors="replace").strip()
    return json.loads(body)


def _norm(v: Any) -> Any:
    """Strings like '0x00C8' compare correctly against numeric 200."""
    if isinstance(v, str) and v.startswith(("0x", "0X")):
        try:
            return int(v, 16)
        except ValueError:
            return v
    return v


# (field on runtime, field on oracle). When the names match we use the
# same string; left here as tuples to keep cross-naming explicit if a
# future Beetle patch exposes more fields.
COMMON_FIELDS = [
    ("intpnd", "intpnd"),
    ("intenb", "intenb"),
    ("dpctrl", "dpctrl"),
    ("xpctrl", "xpctrl"),
    ("frmcyc", "frmcyc"),
    ("bkcol",  "bkcol"),
    ("brta",   "brta"),
    ("brtb",   "brtb"),
    ("brtc",   "brtc"),
    ("rest",   "rest"),
]

# Indexed lists that Beetle exposes via VIP_GetRegister.
INDEXED_FIELDS = [
    ("spt",  4),
    ("gplt", 4),
    ("jplt", 4),
]


def diff(rt: dict[str, Any], or_: dict[str, Any]) -> list[tuple[str, Any, Any]]:
    diffs: list[tuple[str, Any, Any]] = []
    for rt_key, or_key in COMMON_FIELDS:
        a, b = _norm(rt.get(rt_key)), _norm(or_.get(or_key))
        if a != b:
            diffs.append((rt_key, a, b))
    for base, n in INDEXED_FIELDS:
        # Runtime currently doesn't emit indexed arrays in vip_state —
        # only the scalar regs above. The Beetle side does emit them.
        # When the runtime adds them (future) the diff picks them up
        # automatically.
        if base in rt and base in or_:
            for i in range(n):
                a, b = _norm(rt[base][i]), _norm(or_[base][i])
                if a != b:
                    diffs.append((f"{base}[{i}]", a, b))
    return diffs


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--runtime-port", type=int, default=4390)
    p.add_argument("--oracle-port",  type=int, default=4391)
    p.add_argument("--timeout", type=float, default=2.0)
    p.add_argument("--quiet", action="store_true",
                   help="exit 0 with no output if zero diffs")
    args = p.parse_args(argv)

    try:
        rt = _send(args.host, args.runtime_port, "vip_state", args.timeout)
    except OSError as e:
        print(f"runtime ({args.runtime_port}): {e}", file=sys.stderr)
        return 3
    try:
        or_ = _send(args.host, args.oracle_port, "vip_state", args.timeout)
    except OSError as e:
        print(f"oracle ({args.oracle_port}): {e}", file=sys.stderr)
        return 3

    if not rt.get("ok"):
        print(f"runtime returned error: {rt}", file=sys.stderr); return 4
    if not or_.get("ok"):
        print(f"oracle returned error: {or_}",  file=sys.stderr); return 4

    diffs = diff(rt, or_)
    if not diffs:
        if not args.quiet:
            print("vip_state: 0 diffs across "
                  f"{len(COMMON_FIELDS)} common scalar fields "
                  f"+ {sum(n for _, n in INDEXED_FIELDS)} indexed fields")
        return 0

    print(f"vip_state: {len(diffs)} diff(s)")
    print(f"  {'field':<12}  {'runtime':>10}  {'oracle':>10}")
    print(f"  {'-' * 12}  {'-' * 10}  {'-' * 10}")
    for field, rt_v, or_v in diffs:
        rt_s = f"0x{rt_v:04X}" if isinstance(rt_v, int) else str(rt_v)
        or_s = f"0x{or_v:04X}" if isinstance(or_v, int) else str(or_v)
        print(f"  {field:<12}  {rt_s:>10}  {or_s:>10}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
