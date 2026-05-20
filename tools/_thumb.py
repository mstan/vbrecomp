"""ASCII-thumbnail a 32bpp BMP so we can compare framebuffers
without rendering. Used for verification in headless sessions."""
import struct
import sys
from pathlib import Path


def load_bmp_luma(path: Path):
    raw = path.read_bytes()
    pixel_off = struct.unpack_from("<I", raw, 10)[0]
    w = struct.unpack_from("<i", raw, 18)[0]
    h_raw = struct.unpack_from("<i", raw, 22)[0]
    h = abs(h_raw)
    rows = []
    for y in range(h):
        row = []
        for x in range(w):
            i = pixel_off + (y * w + x) * 4
            b, g, r, _ = raw[i:i + 4]
            luma = (r * 299 + g * 587 + b * 114) // 1000
            row.append(luma)
        rows.append(row)
    return w, h, rows


def ascii_thumb(rows, scale=8):
    if not rows:
        return ""
    h = len(rows)
    w = len(rows[0])
    ramp = " .:-=+*#%@"
    out = []
    for y in range(0, h, scale):
        line = []
        for x in range(0, w, scale):
            block = []
            for dy in range(scale):
                for dx in range(scale):
                    if y + dy < h and x + dx < w:
                        block.append(rows[y + dy][x + dx])
            avg = sum(block) // max(len(block), 1)
            line.append(ramp[min(len(ramp) - 1, avg * len(ramp) // 256)])
        out.append("".join(line))
    return "\n".join(out)


def main(argv):
    if not argv:
        print("usage: _thumb.py path.bmp [path.bmp ...]")
        return 2
    for p in argv:
        path = Path(p)
        w, h, rows = load_bmp_luma(path)
        lit = sum(1 for row in rows for px in row if px > 16)
        total = w * h
        print(f"\n=== {path} ({w}x{h}, "
              f"lit pixels: {lit}/{total} = {100 * lit / total:.1f}%) ===")
        print(ascii_thumb(rows, scale=8))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
