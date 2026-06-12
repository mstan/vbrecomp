"""Tiny dependency-free PNG reader for the vbrecomp diff/thumb tools.

The runtime + oracle screenshot commands emit 8-bit PNGs (colour
type 6 / RGBA) via runtime/src/png_write.c. This reader decodes those
— and, for robustness, any 8-bit greyscale / RGB / GA / RGBA PNG —
into a flat list of 0xAARRGGBB pixels using only the standard library.

    from _imgio import load_png
    w, h, px = load_png(Path("vb-runtime-eye0.png"))   # px[i] = 0xAARRGGBB
"""
from __future__ import annotations

import struct
import zlib
from pathlib import Path

_PNG_SIG = b"\x89PNG\r\n\x1a\n"
# channels per colour type
_CHANNELS = {0: 1, 2: 3, 4: 2, 6: 4}


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def load_png(path: Path) -> tuple[int, int, list[int]]:
    """Decode a PNG into (width, height, pixels) where each pixel is
    0xAARRGGBB. Supports 8-bit colour types 0/2/4/6, no interlace."""
    raw = Path(path).read_bytes()
    if raw[:8] != _PNG_SIG:
        raise ValueError(f"{path}: not a PNG")
    pos = 8
    width = height = bit_depth = color_type = interlace = 0
    idat = bytearray()
    while pos + 8 <= len(raw):
        (length,) = struct.unpack_from(">I", raw, pos)
        ctype = raw[pos + 4: pos + 8]
        data = raw[pos + 8: pos + 8 + length]
        pos += 12 + length  # length(4) + type(4) + data + crc(4)
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, _comp, _filt, interlace = \
                struct.unpack_from(">IIBBBBB", data, 0)
        elif ctype == b"IDAT":
            idat += data
        elif ctype == b"IEND":
            break
    if bit_depth != 8:
        raise ValueError(f"{path}: only 8-bit PNGs supported (got {bit_depth})")
    if interlace != 0:
        raise ValueError(f"{path}: interlaced PNGs not supported")
    if color_type not in _CHANNELS:
        raise ValueError(f"{path}: unsupported colour type {color_type}")

    nch = _CHANNELS[color_type]
    stride = width * nch
    rows = zlib.decompress(bytes(idat))
    if len(rows) < height * (1 + stride):
        raise ValueError(f"{path}: truncated image data")

    # Reverse the per-scanline filters into a flat sample buffer.
    out = bytearray(height * stride)
    prev = bytearray(stride)
    o = 0
    for y in range(height):
        ftype = rows[o]; o += 1
        line = bytearray(rows[o:o + stride]); o += stride
        if ftype == 0:
            pass
        elif ftype == 1:  # Sub
            for i in range(nch, stride):
                line[i] = (line[i] + line[i - nch]) & 0xFF
        elif ftype == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ftype == 3:  # Average
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif ftype == 4:  # Paeth
            for i in range(stride):
                a = line[i - nch] if i >= nch else 0
                c = prev[i - nch] if i >= nch else 0
                line[i] = (line[i] + _paeth(a, prev[i], c)) & 0xFF
        else:
            raise ValueError(f"{path}: bad filter type {ftype}")
        out[y * stride:(y + 1) * stride] = line
        prev = line

    # Pack samples into 0xAARRGGBB.
    px: list[int] = []
    if color_type == 6:      # RGBA
        for i in range(0, len(out), 4):
            r, g, b, a = out[i], out[i + 1], out[i + 2], out[i + 3]
            px.append((a << 24) | (r << 16) | (g << 8) | b)
    elif color_type == 2:    # RGB
        for i in range(0, len(out), 3):
            r, g, b = out[i], out[i + 1], out[i + 2]
            px.append((0xFF << 24) | (r << 16) | (g << 8) | b)
    elif color_type == 4:    # grey + alpha
        for i in range(0, len(out), 2):
            v, a = out[i], out[i + 1]
            px.append((a << 24) | (v << 16) | (v << 8) | v)
    else:                    # color_type 0: greyscale
        for v in out:
            px.append((0xFF << 24) | (v << 16) | (v << 8) | v)
    return width, height, px


def luma(p: int) -> int:
    r = (p >> 16) & 0xFF
    g = (p >> 8) & 0xFF
    b = p & 0xFF
    return (r * 299 + g * 587 + b * 114) // 1000
