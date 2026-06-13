/* vip_capture.h — runtime graphics capture (experiment, opt-in).
 *
 * An always-on ring buffer (allocated only when VBRECOMP_CAPTURE is set)
 * that records every unique decoded CHR tile and every per-frame draw-use
 * the VIP renderer assembles, keyed by *content hash* rather than by
 * mutable character-memory address. A TCP command (`capture_dump`) flushes
 * the catalog + per-frame layouts to a human-inspectable directory.
 *
 * Faithfulness contract (see docs/ASSET_CAPTURE.md): with VBRECOMP_CAPTURE
 * unset (default) every entry point here is a no-op — no allocation, no
 * per-frame work, no files written — so rendered output and the oracle
 * compare path are byte-identical. This is debug instrumentation, NOT a
 * behaviour override; it never touches the framebuffer.
 *
 * The capture *pass* (the world/OAM table walk) lives in vip.c so it shares
 * the renderer's exact decode logic; this module owns storage, hashing, and
 * disk output only.
 */
#ifndef VB_VIP_CAPTURE_H
#define VB_VIP_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Draw-use context. OBJ = hardware object/sprite (full 8x8 tile capture);
 * BG / AFFINE = background world (recorded at world granularity for the
 * first proof — see docs). */
typedef enum {
    VB_CAP_CTX_OBJ    = 0,
    VB_CAP_CTX_BG     = 1,
    VB_CAP_CTX_AFFINE = 2,
} VbCaptureCtx;

/* One draw-use: a tile drawn at a position with per-eye placement. All the
 * fields the experiment needs to later reconstruct/replace the asset while
 * preserving position, flip, priority, parallax and per-eye visibility. */
typedef struct {
    uint32_t tile_hash;   /* content hash of the unflipped 8x8 tile (0 = none) */
    uint16_t world_idx;   /* world descriptor index 31..0 (= draw priority)    */
    uint16_t oam_idx;     /* OAM index for OBJ; 0xFFFF for BG/affine            */
    int16_t  x_l, x_r;    /* per-eye screen X (left = jx-jp, right = jx+jp)     */
    int16_t  y;           /* screen Y of the tile's top row                    */
    uint8_t  ctx;         /* VbCaptureCtx                                      */
    uint8_t  vis_l, vis_r;/* per-eye visibility (jlron[lr] && lron[lr])        */
    uint8_t  hflip, vflip;
    uint8_t  palette;     /* JPLT (OBJ) / GPLT (BG) bank 0..3                  */
} VbCaptureUse;

/* Env gate, resolved once (mirrors vb_red_lut_active_kind). 1 = capture on. */
int  vb_capture_active(void);

/* Allocate rings if active; no-op otherwise. Safe to call repeatedly. */
void vb_capture_init(void);
void vb_capture_shutdown(void);

/* Frame bracketing around one capture pass (one drawing frame). */
void vb_capture_begin_frame(void);
void vb_capture_end_frame(void);

/* Content hash of an unflipped 8x8 tile (FNV-1a over its 16 raw 2bpp bytes).
 * The one hash policy shared by the capture pass and the override resolver. */
uint32_t vb_capture_hash(const uint16_t chr16[8]);

/* Dedup-store a tile by content (16 raw 2bpp bytes = 8 halfwords), returning
 * its content hash. `palette`/`ctx` are recorded as secondary metadata on the
 * first sighting. */
uint32_t vb_capture_tile(const uint16_t chr16[8], uint8_t ctx, uint8_t palette);

/* Record one draw-use into the ring + current frame layout. */
void vb_capture_use(const VbCaptureUse* use);

/* Flush the catalog + layouts to `dir` (created if needed). Returns the
 * number of unique tiles written, or -1 on error. */
int  vb_capture_dump(const char* dir);

#ifdef __cplusplus
}
#endif

#endif /* VB_VIP_CAPTURE_H */
