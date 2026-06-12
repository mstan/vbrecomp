/* png_write.h — minimal, dependency-free 32bpp PNG writer.
 *
 * Shared by the runtime and oracle screenshot commands so both
 * processes emit comparable PNGs (replaces the old BMP writers).
 *
 * The encoder writes an 8-bit RGBA (PNG colour type 6), top-down
 * image from an ARGB (0xAARRGGBB) pixel buffer. Compression uses
 * zlib "stored" (uncompressed) deflate blocks — no libz/libpng link
 * dependency, at the cost of a slightly larger file. Frame dumps are
 * one-shot debug artifacts, so size does not matter.
 */
#ifndef VB_PNG_WRITE_H
#define VB_PNG_WRITE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Write `argb` (w*h pixels, 0xAARRGGBB, row-major top-down) to `path`
 * as an 8-bit RGBA PNG. Returns 0 on success, non-zero on failure. */
int vb_write_png_32bpp(const char* path, int w, int h, const uint32_t* argb);

#ifdef __cplusplus
}
#endif

#endif /* VB_PNG_WRITE_H */
