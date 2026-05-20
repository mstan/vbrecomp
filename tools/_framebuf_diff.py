"""Cross-process framebuffer diff: vb-runtime vs vb-beetle.

Per CLAUDE.md Rule 14 the two processes are independent — we sample
state and diff. This tool:
  1. Asks vb-runtime to dump its current display-FB to a .bmp
  2. Asks vb-beetle to dump its current libretro framebuffer to a .bmp
  3. Loads both BMPs, compares pixel-by-pixel for the overlapping
     384×224 left-eye region
  4. Reports total differing pixels + first divergent (x, y) + a
     reproducible image of the diff overlay

Differences in 3D mode (anaglyph vs separate L/R) are handled by
treating both as single-image inputs and comparing the left 384
columns of Beetle's output against the runtime's left eye.

    python tools/_framebuf_diff.py
    python tools/_framebuf_diff.py --tolerance 4
"""
from __future__ import annotations

import argparse
import json
import socket
import struct
import sys
from pathlib import Path


def _send(host: str, port: int, cmd: dict, timeout: float = 5.0) -> dict:
    with socket.create_connection((host, port), timeout=timeout) as s:
        s.sendall((json.dumps(cmd) + "\n").encode("ascii"))
        data = b""
        while not data.endswith(b"\n"):
            chunk = s.recv(8192)
            if not chunk:
                break
            data += chunk
    return json.loads(data.decode("ascii", errors="replace").strip())


def _load_bmp(path: Path) -> tuple[int, int, list[int]]:
    """Read a BI_RGB 32bpp BMP and return (w, h, pixels[]) with each
    pixel as 0xAARRGGBB. Tolerates positive or negative height."""
    raw = path.read_bytes()
    if len(raw) < 54 or raw[:2] != b"BM":
        raise ValueError(f"{path}: not a BMP")
    pixel_off = struct.unpack_from("<I", raw, 10)[0]
    w = struct.unpack_from("<i", raw, 18)[0]
    h = struct.unpack_from("<i", raw, 22)[0]
    bpp = struct.unpack_from("<H", raw, 28)[0]
    if bpp != 32:
        raise ValueError(f"{path}: expected 32bpp, got {bpp}")
    flipped = h > 0   # positive = bottom-up
    abs_h = abs(h)
    px = []
    for i in range(w * abs_h):
        b, g, r, a = raw[pixel_off + i * 4: pixel_off + i * 4 + 4]
        px.append((a << 24) | (r << 16) | (g << 8) | b)
    if flipped:
        # reverse row order
        rows = [px[i * w:(i + 1) * w] for i in range(abs_h)]
        rows.reverse()
        px = [p for row in rows for p in row]
    return w, abs_h, px


def _luma(p: int) -> int:
    r = (p >> 16) & 0xFF
    g = (p >>  8) & 0xFF
    b = (p      ) & 0xFF
    return (r * 299 + g * 587 + b * 114) // 1000


def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--host", default="127.0.0.1")
    p.add_argument("--runtime-port", type=int, default=4390)
    p.add_argument("--oracle-port",  type=int, default=4391)
    p.add_argument("--tolerance", type=int, default=0,
                   help="per-channel luma tolerance (0 = exact)")
    p.add_argument("--runtime-path", default="vb-runtime-eye0.bmp")
    p.add_argument("--oracle-path",  default="vb-beetle-fb.bmp")
    p.add_argument("--keep", action="store_true",
                   help="don't delete the .bmp files after diffing")
    p.add_argument("--timeout", type=float, default=5.0)
    args = p.parse_args(argv)

    try:
        rt = _send(args.host, args.runtime_port,
                   {"cmd": "screenshot", "id": 1,
                    "path": args.runtime_path, "eye": 0},
                   args.timeout)
    except OSError as e:
        print(f"runtime ({args.runtime_port}): {e}", file=sys.stderr); return 3
    try:
        or_ = _send(args.host, args.oracle_port,
                    {"cmd": "screenshot", "id": 1,
                     "path": args.oracle_path},
                    args.timeout)
    except OSError as e:
        print(f"oracle ({args.oracle_port}): {e}", file=sys.stderr); return 3

    if not rt.get("ok"):
        print(f"runtime returned error: {rt}", file=sys.stderr); return 4
    if not or_.get("ok"):
        print(f"oracle returned error: {or_}", file=sys.stderr); return 4

    rt_w, rt_h, rt_px = _load_bmp(Path(args.runtime_path))
    or_w, or_h, or_px = _load_bmp(Path(args.oracle_path))

    if rt_w != 384 or rt_h != 224:
        print(f"runtime: unexpected dims {rt_w}x{rt_h}", file=sys.stderr); return 5

    # Beetle's libretro output dimensions vary by 3D mode. For
    # anaglyph the typical output is 384×224 stereo-combined; for
    # side-by-side it's 768×224. We compare against the LEFT 384
    # columns of whatever Beetle gave us.
    if or_w < 384 or or_h < 224:
        print(f"oracle: framebuffer too small {or_w}x{or_h} "
              f"(expected at least 384x224)", file=sys.stderr); return 5

    width = 384
    height = 224
    diffs = 0
    first_diff: tuple[int, int, int, int] | None = None
    for y in range(height):
        for x in range(width):
            rt_p = rt_px[y * rt_w + x]
            or_p = or_px[y * or_w + x]
            if args.tolerance == 0:
                if rt_p != or_p:
                    diffs += 1
                    if first_diff is None:
                        first_diff = (x, y, rt_p, or_p)
            else:
                if abs(_luma(rt_p) - _luma(or_p)) > args.tolerance:
                    diffs += 1
                    if first_diff is None:
                        first_diff = (x, y, rt_p, or_p)

    total = width * height
    pct = (100.0 * diffs / total) if total else 0.0
    print(f"framebuf_diff: {diffs}/{total} pixels differ ({pct:.2f}%)")
    if first_diff is not None:
        x, y, rt_p, or_p = first_diff
        print(f"  first diff at ({x}, {y}): "
              f"runtime=0x{rt_p:08X} oracle=0x{or_p:08X}")
    if not args.keep:
        for path in (args.runtime_path, args.oracle_path):
            try:
                Path(path).unlink()
            except OSError:
                pass
    return 0 if diffs == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
