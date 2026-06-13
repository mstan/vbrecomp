/* recolor.h — opt-in present-time full-screen recolor (experiment).
 *
 * The VIP rasterizes every world (OBJ/BG/affine) into a 2bpp brightness value.
 * This layer tags each rasterized pixel with the identity of the source CHR
 * tile that drew it (content hash + palette bank), then maps
 * (identity, brightness 0..3) -> RGB at present time. Because it recolors the
 * final rasterized pixels it works regardless of affine perspective warp, and
 * because identity is the *content hash* (not VRAM slot / world index) the same
 * colors follow a character across scenes.
 *
 * Faithful by default (mirrors red_lut / docs/ASSET_CAPTURE.md): active only
 * when VBRECOMP_OVERRIDES points at an overrides dir containing a parseable
 * recolor/palette.json with >=1 entry. When inactive, no attribution is written
 * and the present path uses the faithful red render — byte-identical, oracle-safe.
 * A missing/empty/malformed pack falls back to faithful and never aborts.
 *
 * Attribution id = (char_no & 0x7FF) | (palette << 11), 13 bits, 0..8191.
 */
#ifndef VB_RECOLOR_H
#define VB_RECOLOR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Env gate (VBRECOMP_OVERRIDES dir + recolor/palette.json), resolved once. */
int  vb_recolor_active(void);

/* Load the color pack if active; safe to re-call. Never aborts. */
void vb_recolor_init(void);
void vb_recolor_shutdown(void);

/* Re-read the pack file at runtime (debug: author a pack live, then reload).
 * Keeps the active state on so attribution/present keep flowing. */
void vb_recolor_reload(void);

/* Number of pack entries loaded (introspection). */
int  vb_recolor_entry_count(void);

/* Per-frame LUT lifecycle (called from the VIP resolve pass). */
void vb_recolor_frame_reset(void);
/* If the pack has an entry for (hash, palette), install its RGB ramp into the
 * per-frame LUT slot `id`. No-op otherwise. */
void vb_recolor_resolve(uint16_t id, uint32_t hash, int palette);

/* Present: if `id` has a recolor this frame, write the ARGB for brightness
 * `value` (0..3) and return 1; else return 0 (caller renders faithfully). */
int  vb_recolor_pixel(uint16_t id, int value, uint32_t* argb_out);

#ifdef __cplusplus
}
#endif

#endif /* VB_RECOLOR_H */
