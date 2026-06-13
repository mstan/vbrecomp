/* recolor.h — opt-in present-time full-screen recolor (experiment).
 *
 * The VIP rasterizes every world (OBJ/BG/affine) into a 2bpp brightness value.
 * This layer tags each rasterized pixel with the **world index** that drew it,
 * then at present maps (world, vertical-position-within-that-world, brightness)
 * -> RGB. Keying on the world (not the CHR slot or tile content) makes a
 * character stay colored through its whole animation: the world that draws the
 * near player is stable across frames even as its tiles change. Per-world
 * vertical "body bands" give real per-part colors (cap / shirt / overalls /
 * shoes). Works regardless of affine perspective warp (it recolors the final
 * rasterized pixels).
 *
 * Faithful by default (mirrors red_lut / docs/ASSET_CAPTURE.md): active only
 * when VBRECOMP_OVERRIDES points at an overrides dir with a parseable
 * recolor/palette.json. When inactive, no attribution is written and present
 * uses the faithful red render — byte-identical, oracle-safe. A
 * missing/empty/malformed pack falls back to faithful and never aborts.
 *
 * Note: world index is stable across a scene's animation but NOT across
 * different scenes (world 24 = the player in a match, something else on a
 * menu). Packs are therefore authored per scene.
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

/* Re-read the pack file at runtime (author live, then reload). */
void vb_recolor_reload(void);

/* Number of world rules loaded (introspection). */
int  vb_recolor_entry_count(void);

/* Present-time pixel color for a world-attributed pixel. `world` is the world
 * index 0..31; `rel_num`/`rel_den` give the pixel's vertical position within
 * that world's on-screen bounding box (rel = rel_num/rel_den in [0,1));
 * `value` is the 2bpp brightness 0..3. If the pack has a rule for `world` (and
 * a band covering rel), writes the ARGB and returns 1; else returns 0 (caller
 * renders faithfully). */
int  vb_recolor_world_pixel(int world, int rel_num, int rel_den, int value,
                            uint32_t* argb_out);

#ifdef __cplusplus
}
#endif

#endif /* VB_RECOLOR_H */
