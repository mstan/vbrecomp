/* png_read.h — minimal RGBA PNG decoder (sibling of png_write.c).
 *
 * Reads 8-bit RGBA (colour type 6) or RGB (type 2) PNGs whose IDAT zlib
 * stream uses *stored* (uncompressed) deflate blocks — the exact format
 * png_write.c emits and that tools/colorize_tile.py writes. This keeps the
 * override layer self-contained with no zlib dependency, matching the
 * project's existing "no external compression library" choice (see
 * png_write.c). PNGs using compressed deflate blocks are rejected
 * (returns -1) rather than mis-decoded — the caller then falls back to the
 * faithful renderer for that asset.
 *
 * Output is ARGB8888 (0xAARRGGBB) so it composites directly over the
 * framebuffer the present path produces.
 */
#ifndef VB_PNG_READ_H
#define VB_PNG_READ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Decode `path` into a freshly malloc'd ARGB8888 buffer. On success returns
 * 0 and sets *out_argb (caller frees), *out_w, *out_h. On any error returns
 * non-zero and leaves outputs untouched. */
int vb_read_png_rgba(const char* path, uint32_t** out_argb,
                     int* out_w, int* out_h);

#ifdef __cplusplus
}
#endif

#endif /* VB_PNG_READ_H */
