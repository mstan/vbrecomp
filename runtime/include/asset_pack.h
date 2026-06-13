/* asset_pack.h — opt-in graphics override pack (experiment).
 *
 * Loads a per-game graphics override manifest + RGBA replacement images and
 * exposes (a) content-hash lookup used by the VIP resolve pass to suppress a
 * matched OBJ tile, and (b) a double-buffered overlay draw-list the present
 * path composites per eye over the faithful framebuffer.
 *
 * Extensibility contract (mirrors snesrecomp overrides/, see
 * docs/ASSET_CAPTURE.md): faithful by default. With VBRECOMP_OVERRIDES unset
 * nothing loads and every entry point is inert, so output is byte-identical.
 * An override engages for a draw only when the env is set AND a manifest
 * entry matches that tile's content hash; a missing/invalid manifest or a
 * missing image falls back to faithful for that item and never aborts. The
 * generic engine is game-agnostic — the manifest + images live in the game
 * repo under overrides/graphics/.
 */
#ifndef VB_ASSET_PACK_H
#define VB_ASSET_PACK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* A decoded RGBA replacement image, keyed by the content hash of the
 * UNFLIPPED 8x8 source tile it replaces. */
typedef struct {
    uint32_t  hash;
    int       w, h;
    int       anchor_x, anchor_y;  /* draw offset from the tile's top-left */
    uint32_t* argb;                /* 0xAARRGGBB, w*h */
} VbOverrideImage;

/* One per-eye overlay blit queued by the resolve pass, consumed at present. */
typedef struct {
    const VbOverrideImage* img;
    int16_t x_l, x_r, y;
    uint8_t hflip, vflip, vis_l, vis_r;
} VbOverlayCmd;

/* Env gate (VBRECOMP_OVERRIDES = path to the overrides/ dir), resolved once. */
int  vb_overrides_active(void);

/* Load overrides/graphics/manifest.json + images if active; safe to re-call.
 * Never aborts: any failure leaves the pack empty (faithful). */
void vb_overrides_init(void);
void vb_overrides_shutdown(void);

/* Returns the replacement image for an unflipped-tile content hash, or NULL
 * if there is none (caller renders faithfully). */
const VbOverrideImage* vb_overrides_lookup_tile(uint32_t tile_hash);

/* Number of replacement images successfully loaded (introspection). */
int vb_overrides_image_count(void);

/* Double-buffered overlay draw-list, indexed by framebuffer slot (0/1) so the
 * list built while drawing buffer N is consumed when buffer N is displayed. */
void vb_overlay_reset(int slot);
void vb_overlay_add(int slot, const VbOverlayCmd* cmd);
int  vb_overlay_count(int slot);
const VbOverlayCmd* vb_overlay_cmds(int slot);

/* Alpha-composite the overlay list `slot` into one ARGB8888 eye buffer
 * (w*h, typically 384x224) for `eye` (0 = left uses x_l, 1 = right uses x_r),
 * honoring per-eye visibility and hflip/vflip. No-op when the list is empty,
 * so callers stay faithful by default. Used by both the present path and the
 * (opt-in) enhanced screenshot. */
void vb_overlay_composite(uint32_t* eye_buf, int w, int h, int eye, int slot);

#ifdef __cplusplus
}
#endif

#endif /* VB_ASSET_PACK_H */
