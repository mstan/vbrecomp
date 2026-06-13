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
 * menu). A pack therefore declares multiple *scenes*, each with a detection
 * signature (the set of world indices that must be present / absent on screen)
 * and its own world rules. Each frame the runtime computes the active-world
 * mask and selects the first matching scene; if none match, the frame renders
 * faithfully. The legacy flat form ({"worlds":[...]}) is still accepted and
 * treated as one always-matching scene.
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

/* Total world rules loaded across all scenes (introspection). */
int  vb_recolor_entry_count(void);

/* Number of scenes loaded (introspection). */
int  vb_recolor_scene_count(void);

/* Select the active scene from the set of world indices present on screen this
 * frame (`active_world_mask` bit i set == world index i present). Picks the
 * first scene whose detection signature matches; sets it as current and
 * returns its index, or -1 if none match (frame then renders faithfully).
 * Call once per frame before the vb_recolor_world_pixel pixel loop. */
int  vb_recolor_select_scene(uint32_t active_world_mask);

/* Name of the currently-selected scene, or "" if none (introspection). */
const char* vb_recolor_current_scene(void);

/* Scene name by index, or "" if out of range (introspection). */
const char* vb_recolor_scene_name(int idx);

/* Always-on scene-selection decision ring (i=0 oldest .. len-1 newest). Each
 * vb_recolor_select_scene call appends {seq, active-world mask, chosen scene
 * (-1=none)}. Query the window of interest after the fact — do not arm. */
int vb_recolor_trace_len(void);
int vb_recolor_trace_get(int i, uint32_t* seq, uint32_t* mask, int* scene);

/* Present-time pixel color for a world-attributed pixel, resolved within the
 * scene chosen by the last vb_recolor_select_scene call. `world` is the world
 * index 0..31. `relx_num`/`relx_den` and `rely_num`/`rely_den` give the
 * pixel's horizontal and vertical position within that world's on-screen
 * bounding box (each rel = num/den in [0,1)); the rule's horizontal columns
 * select on rel-x, then its vertical bands select on rel-y. `value` is the
 * 2bpp brightness 0..3. If the current scene has a rule for `world` (and a
 * column+band covering the pixel), writes the ARGB and returns 1; else returns
 * 0 (caller renders faithfully). A rule with no horizontal split is one
 * full-width column, so rel-x is ignored for the common vertical-only case. */
int  vb_recolor_world_pixel(int world, int relx_num, int relx_den,
                            int rely_num, int rely_den, int value,
                            uint32_t* argb_out);

#ifdef __cplusplus
}
#endif

#endif /* VB_RECOLOR_H */
