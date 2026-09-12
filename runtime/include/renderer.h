#ifndef VB_RENDERER_H
#define VB_RENDERER_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define VB_RENDER_WIDTH 384
#define VB_RENDER_HEIGHT 224
#define VB_RENDER_PIXELS (VB_RENDER_WIDTH * VB_RENDER_HEIGHT)

/* Source artwork sampled by the native rasterizer. Recorded at draw time,
 * not reconstructed from VRAM after display. x/y are background-map texels;
 * u/v are unflipped CHR texels. kind 3 denotes OBJ, with map=255.
 * kind 4 is a changed CPU framebuffer pixel: tile_hash stores the opaque
 * game-owned provenance tag, x/y are framebuffer coordinates, map=255,
 * world=0. Tag 0 means unclassified. No CHR identity exists for kind 4. */
typedef struct VbSourceTexel {
    uint32_t tile_hash;
    uint16_t x, y, tile;
    uint8_t u, v, map, kind, raw, world;
} VbSourceTexel;

/* Borrowed, read-only inputs valid for one callback. World attribution uses
 * 0 for backdrop, 1..32 for VIP world 0..31. Levels are unpacked native 2bpp.
 * A custom renderer changes presentation only: no guest timing or ROM writes.
 * frame_seq identifies displayed guest frames; eye is 0 (left) or 1 (right).
 */
typedef struct VbRenderFrame {
    uint32_t frame_seq;
    int eye;
    const uint32_t* stock_argb;
    const uint8_t* levels;
    const uint16_t* worlds;
    const VbSourceTexel* sources;
    int width, height; /* Presentation extent; native path remains 384x224. */
} VbRenderFrame;
typedef void (*VbRenderCallback)(const VbRenderFrame*, uint32_t* argb, void* context);

/* A single active renderer. Return 0 rather than silently replacing another
 * plugin's renderer. Register from activation; unregister from reset callback.
 */
int vb_renderer_register(const char* id, VbRenderCallback render, void* context);
/* Game startup declares this once so enabling a renderer while paused has
 * attribution for the already displayed frame, even if mods started disabled. */
void vb_renderer_track_worlds(void);
int vb_renderer_tracks_worlds(void);
void vb_renderer_track_texels(void);
int vb_renderer_tracks_texels(void);
void vb_renderer_reset(void);
int vb_renderer_active(void);
const char* vb_renderer_id(void);
void vb_renderer_present(int eye, uint32_t* argb);
/* Variable-width presentation composes the viewport with the color callback.
 * Native present() remains fixed-size for existing tools and raw comparisons. */
int vb_renderer_present_width(void);
int vb_renderer_present_viewport(int eye, int width, uint32_t* argb);
const VbSourceTexel* vb_renderer_present_sources(int eye, int width);

#ifdef __cplusplus
}
#endif
#endif
