"""ASCII-thumbnail a screenshot PNG so we can compare framebuffers
without rendering. Used for verification in headless sessions."""
import sys
from pathlib import Path

from _imgio import load_png, luma


def load_fb_luma(path: Path):
    w, h, px = load_png(path)
    rows = [[luma(px[y * w + x]) for x in range(w)] for y in range(h)]
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
        print("usage: _thumb.py path.png [path.png ...]")
        return 2
    for p in argv:
        path = Path(p)
        w, h, rows = load_fb_luma(path)
        lit = sum(1 for row in rows for px in row if px > 16)
        total = w * h
        print(f"\n=== {path} ({w}x{h}, "
              f"lit pixels: {lit}/{total} = {100 * lit / total:.1f}%) ===")
        print(ascii_thumb(rows, scale=8))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
