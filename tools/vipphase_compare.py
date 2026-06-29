"""vipphase_compare.py — VIP draw-timing phase diff: recomp vs oracle.

Axis-5a phase gate. Both vb-runtime (4390) and vb-beetle (4391) snapshot
DPSTTS (display status) and XPSTTS (drawing status) into an always-on,
first-N-from-boot ring at each deterministic VIP draw-timing event
(FRAME_START / GAME_START / XP_END / L|R FB_END). Because both VIPs are
freshly advanced at an event boundary, the snapshot is well-defined — unlike
an uncontrolled instantaneous read of these transient status registers, which
is pure sampling noise.

This QUERIES both rings (Rule 3 — never arms a capture), aligns them by event
sequence (the event TYPE at each index must match; both VIPs emit the same
draw-timing event order), and reports the first index where the event type or
the DPSTTS/XPSTTS value diverges — i.e. the first draw-timing PHASE
divergence, cycle/phase-aligned by construction.

    python tools/vipphase_compare.py --rom roms/marios_tennis.vb
    python tools/vipphase_compare.py --runtime-port 4390 --oracle-port 4391 --events 200000
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

REC = 16  # bytes: seq(u64) event(u16) dpstts(u16) xpstts(u16) reserved(u16)
EV = {0x0002: "LFBEND", 0x0004: "RFBEND", 0x0008: "GAMESTART",
      0x0010: "FRAMESTART", 0x2000: "SBHIT", 0x4000: "XPEND",
      0x0001: "SCANERR", 0x8000: "TIMEERR"}


def _send(host: str, port: int, cmd: dict, timeout: float = 5.0) -> dict:
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
    """Pull [0, want) records from a vip_phase ring, paging by max."""
    out: list[tuple] = []
    start = 0
    while start < want:
        r = _send(host, port,
                  {"cmd": "vip_phase", "id": 1, "start": start, "max": 65536},
                  timeout)
        if not r.get("ok"):
            raise RuntimeError(f"{port}: {r}")
        raw = bytes.fromhex(r.get("hex", ""))
        got = r.get("returned", 0)
        if got == 0:
            break
        for i in range(0, len(raw), REC):
            seq, ev, dp, xp, _ = struct.unpack_from("<QHHHH", raw, i)
            out.append((seq, ev, dp, xp))
        start += got
        if got < 65536:
            break
    return out


def wait_head(host: str, port: int, target: int, timeout_s: float) -> int:
    """Wait until the ring head reaches `target` events (or time out)."""
    deadline = time.time() + timeout_s
    head = 0
    while time.time() < deadline:
        r = _send(host, port, {"cmd": "vip_phase", "id": 1, "start": 0, "max": 1})
        head = r.get("head", 0)
        if head >= target:
            return head
        time.sleep(0.2)
    return head


def evname(e: int) -> str:
    return EV.get(e, f"0x{e:04X}")


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--runtime-port", type=int, default=4390)
    p.add_argument("--oracle-port", type=int, default=4391)
    p.add_argument("--events", type=int, default=200000,
                   help="target number of from-boot events to compare")
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

        # Both rings are first-N-from-boot; wait until each holds enough.
        rt_head = wait_head(args.host, args.runtime_port, args.events, 40)
        or_head = wait_head(args.host, args.oracle_port, args.events, 40)
        n = min(args.events, rt_head, or_head)
        print(f"comparing [0,{n}) events  (runtime head={rt_head} "
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
    ev_mismatch = dp_mismatch = xp_mismatch = 0
    first = None
    frame = 0
    first_frame = None
    for i in range(m):
        _, rev, rdp, rxp = rt[i]
        _, oev, odp, oxp = orc[i]
        if rev == 0x0010:
            frame += 1
        bad = (rev != oev) or (rdp != odp) or (rxp != oxp)
        if bad and first is None:
            first = (i, frame, (rev, rdp, rxp), (oev, odp, oxp))
            first_frame = frame
        if rev != oev:
            ev_mismatch += 1
        if rdp != odp:
            dp_mismatch += 1
        if rxp != oxp:
            xp_mismatch += 1

    print("=" * 66)
    print("  VB VIP DRAW-TIMING PHASE — recomp (4390) vs oracle (4391)")
    print("=" * 66)
    print(f"  events compared      {m}   (~{frame} display frames)")
    print(f"  event-type mismatch  {ev_mismatch}")
    print(f"  DPSTTS mismatch      {dp_mismatch}")
    print(f"  XPSTTS mismatch      {xp_mismatch}")
    print("-" * 66)
    if first is None:
        print(f"  VERDICT: PHASE MATCH — all {m} draw-timing events identical "
              f"(event, DPSTTS, XPSTTS) from boot.")
        rc = 0
    else:
        i, fr, (rev, rdp, rxp), (oev, odp, oxp) = first
        print(f"  VERDICT: first divergence at event #{i} (~frame {fr})")
        print(f"    event   recomp={evname(rev):<10} oracle={evname(oev)}")
        print(f"    DPSTTS  recomp=0x{rdp:04X}    oracle=0x{odp:04X}"
              f"    xor=0x{rdp ^ odp:04X}")
        print(f"    XPSTTS  recomp=0x{rxp:04X}    oracle=0x{oxp:04X}"
              f"    xor=0x{rxp ^ oxp:04X}")
        rc = 1
    print("=" * 66)
    return rc


if __name__ == "__main__":
    sys.exit(main())
