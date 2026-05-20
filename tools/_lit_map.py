"""Dump lit-pixel coordinates from a 32bpp BMP so we can see WHERE
the content is. Also reports the bounding box."""
import struct
import sys
from pathlib import Path


def main(argv):
    if len(argv) != 1:
        print("usage: _lit_map.py path.bmp")
        return 2
    raw = Path(argv[0]).read_bytes()
    pixel_off = struct.unpack_from("<I", raw, 10)[0]
    w = struct.unpack_from("<i", raw, 18)[0]
    h = abs(struct.unpack_from("<i", raw, 22)[0])

    lit = []
    for y in range(h):
        for x in range(w):
            i = pixel_off + (y * w + x) * 4
            b, g, r, _ = raw[i:i + 4]
            luma = (r * 299 + g * 587 + b * 114) // 1000
            if luma > 4:
                lit.append((x, y, r, g, b, luma))

    if not lit:
        print("0 lit pixels")
        return 0

    xs = [p[0] for p in lit]
    ys = [p[1] for p in lit]
    print(f"{len(lit)} lit pixels")
    print(f"bbox: x=[{min(xs)},{max(xs)}] y=[{min(ys)},{max(ys)}]")
    print(f"sample rgb: r=[{min(p[2] for p in lit)},{max(p[2] for p in lit)}] "
          f"g=[{min(p[3] for p in lit)},{max(p[3] for p in lit)}] "
          f"b=[{min(p[4] for p in lit)},{max(p[4] for p in lit)}]")

    # Render a 2x downscale dot map within the bbox area
    minx, maxx = min(xs), max(xs)
    miny, maxy = min(ys), max(ys)
    bw = maxx - minx + 1
    bh = maxy - miny + 1
    grid = [[0] * bw for _ in range(bh)]
    for x, y, *_ in lit:
        grid[y - miny][x - minx] = 1
    scale = max(1, max(bw, bh) // 80)
    print(f"\nDot map ({bw}x{bh}, scale 1:{scale}):")
    for y in range(0, bh, scale):
        line = []
        for x in range(0, bw, scale):
            block = 0
            for dy in range(scale):
                for dx in range(scale):
                    if y + dy < bh and x + dx < bw:
                        block += grid[y + dy][x + dx]
            line.append("#" if block > scale * scale // 4 else
                       ":" if block > 0 else " ")
        print("  " + "".join(line))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
