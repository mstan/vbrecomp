#include "renderer.h"
#include "vip.h"
#include "viewport.h"
#include <string.h>

static VbRenderCallback s_render;
static void* s_context;
static char s_id[96];
static int s_track_worlds;
static int s_track_texels;
void vb_renderer_track_texels(void) { s_track_texels = s_track_worlds = 1; }
int vb_renderer_tracks_texels(void) { return s_track_texels; }
void vb_renderer_track_worlds(void) { s_track_worlds = 1; }
int vb_renderer_tracks_worlds(void) { return s_track_worlds; }

int vb_renderer_register(const char* id, VbRenderCallback render, void* context) {
    if (!id || !*id || strlen(id) >= sizeof(s_id) || !render || s_render) return 0;
    strcpy(s_id, id);
    s_render = render;
    s_context = context;
    return 1;
}
void vb_renderer_reset(void) { s_render = 0; s_context = 0; s_id[0] = 0; vb_viewport_reset(); }
int vb_renderer_active(void) { return s_render != 0 || vb_viewport_active(); }
const char* vb_renderer_id(void) { return s_id; }

void vb_renderer_present(int eye, uint32_t* argb) {
    static uint32_t stock[VB_RENDER_PIXELS];
    static uint8_t levels[VB_RENDER_PIXELS];
    if (!argb) return;
    vb_vip_render_framebuffer(eye, argb);
    if (!s_render) return;
    memcpy(stock, argb, sizeof(stock));
    vb_vip_copy_levels(eye, levels);
    const VbRenderFrame frame = {
        vb_vip_frame_seq(), eye != 0, stock, levels, vb_vip_attr_buffer(eye),
        vb_renderer_tracks_texels() ? vb_vip_source_buffer(eye) : 0,
        VB_RENDER_WIDTH, VB_RENDER_HEIGHT
    };
    s_render(&frame, argb, s_context);
}

static uint32_t view_stock[VB_VIEWPORT_MAX_WIDTH * VB_RENDER_HEIGHT];
static uint8_t view_levels[VB_VIEWPORT_MAX_WIDTH * VB_RENDER_HEIGHT];
static uint16_t view_worlds[VB_VIEWPORT_MAX_WIDTH * VB_RENDER_HEIGHT];
static VbSourceTexel view_sources[VB_VIEWPORT_MAX_WIDTH * VB_RENDER_HEIGHT];
int vb_renderer_present_width(void) { return vb_viewport_width(); }
int vb_renderer_present_viewport(int eye, int width, uint32_t* argb) {
    if (!argb) return 0;
    if (width == VB_RENDER_WIDTH) { vb_renderer_present(eye, argb); return 1; }
    if (!vb_viewport_active() || !vb_viewport_render(vb_vip_display_fb() & 1,
        eye, width, view_stock, view_levels, view_worlds, view_sources)) return 0;
    memcpy(argb, view_stock, width * VB_RENDER_HEIGHT * sizeof(*argb));
    if (s_render) {
        const VbRenderFrame frame = {vb_vip_frame_seq(), eye != 0, view_stock,
            view_levels, view_worlds, view_sources, width, VB_RENDER_HEIGHT};
        s_render(&frame, argb, s_context);
    }
    return 1;
}
const VbSourceTexel* vb_renderer_present_sources(int eye, int width) {
    return width == VB_RENDER_WIDTH ? vb_vip_source_buffer(eye) : view_sources;
}
