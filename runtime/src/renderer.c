#include "renderer.h"
#include "vip.h"
#include <string.h>

static VbRenderCallback s_render;
static void* s_context;
static char s_id[96];
static int s_track_worlds;
void vb_renderer_track_worlds(void) { s_track_worlds = 1; }
int vb_renderer_tracks_worlds(void) { return s_track_worlds; }

int vb_renderer_register(const char* id, VbRenderCallback render, void* context) {
    if (!id || !*id || strlen(id) >= sizeof(s_id) || !render || s_render) return 0;
    strcpy(s_id, id);
    s_render = render;
    s_context = context;
    return 1;
}
void vb_renderer_reset(void) { s_render = 0; s_context = 0; s_id[0] = 0; }
int vb_renderer_active(void) { return s_render != 0; }
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
        vb_vip_frame_seq(), eye != 0, stock, levels, vb_vip_attr_buffer(eye)
    };
    s_render(&frame, argb, s_context);
}
