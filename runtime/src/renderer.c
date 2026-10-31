#include "renderer.h"
#include "vip.h"
#include "viewport.h"
#include <string.h>

static VbRenderCallback s_render;
static void* s_context;
static char s_id[96];
static int s_track_worlds;
static int s_track_texels;
static int s_local_pixels;
static uint32_t native_stock[VB_RENDER_PIXELS];
#ifndef VB_STREAM_SHADING
#define VB_STREAM_SHADING 1
#endif
void vb_renderer_track_texels(void) { s_track_texels = s_track_worlds = 1; }
int vb_renderer_tracks_texels(void) { return s_track_texels; }
void vb_renderer_track_worlds(void) { s_track_worlds = 1; }
int vb_renderer_tracks_worlds(void) { return s_track_worlds; }

int vb_renderer_register(const char* id, VbRenderCallback render, void* context) {
    if (!id || !*id || strlen(id) >= sizeof(s_id) || !render || s_render) return 0;
    strcpy(s_id, id);
    s_render = render;
    s_context = context;
    s_local_pixels = 0;
    return 1;
}
int vb_renderer_register_local_pixels(const char* id,VbRenderCallback render,void* context) {
    if(!vb_renderer_register(id,render,context))return 0;
    s_local_pixels=1;return 1;
}
void vb_renderer_reset(void) { s_render = 0; s_context = 0; s_id[0] = 0; s_local_pixels=0; vb_viewport_reset(); }
int vb_renderer_active(void) { return s_render != 0 || vb_viewport_active(); }
const char* vb_renderer_id(void) { return s_id; }
static void apply_local_rows(const VbRenderFrame* frame,uint32_t* out) {
    uint32_t stock[VB_RENDER_WIDTH];
    for(unsigned y=0;y<VB_RENDER_HEIGHT;++y) {
        const unsigned at=y*VB_RENDER_WIDTH;
        memcpy(stock,frame->stock_argb+at,sizeof(stock));
        if(out!=frame->stock_argb)memcpy(out+at,stock,sizeof(stock));
        VbRenderFrame row=*frame;
        row.stock_argb=stock;row.height=1;
        row.levels=frame->levels?frame->levels+at:0;
        row.worlds=frame->worlds?frame->worlds+at:0;
        row.sources=frame->sources?frame->sources+at:0;
        s_render(&row,out+at,s_context);
    }
}
int vb_renderer_apply_frame(const VbRenderFrame* frame,uint32_t* out) {
    if(!frame || !out || !frame->stock_argb || frame->width!=VB_RENDER_WIDTH
       || frame->height!=VB_RENDER_HEIGHT || (frame->eye!=0 && frame->eye!=1))return 0;
    if(VB_STREAM_SHADING && s_render && s_local_pixels){apply_local_rows(frame,out);return 1;}
    if(s_render && s_local_pixels && out==frame->stock_argb) {
        VbRenderFrame copy=*frame;
        memcpy(native_stock,frame->stock_argb,sizeof(native_stock));copy.stock_argb=native_stock;
        s_render(&copy,out,s_context);return 1;
    }
    if(out!=frame->stock_argb)memcpy(out,frame->stock_argb,VB_RENDER_PIXELS*sizeof(*out));
    if(s_render)s_render(frame,out,s_context);
    return 1;
}

void vb_renderer_present(int eye, uint32_t* argb) {
    static uint8_t levels[VB_RENDER_PIXELS];
    if (!argb) return;
    if (!s_render) { vb_vip_render_framebuffer(eye,argb); return; }
    vb_vip_render_framebuffer_levels(eye,argb,levels);
    const VbRenderFrame frame = {
        vb_vip_frame_seq(), eye != 0, (VB_STREAM_SHADING&&s_local_pixels)?argb:native_stock, levels, vb_vip_attr_buffer(eye),
        vb_renderer_tracks_texels() ? vb_vip_source_buffer(eye) : 0,
        VB_RENDER_WIDTH, VB_RENDER_HEIGHT
    };
    if(VB_STREAM_SHADING && s_local_pixels){apply_local_rows(&frame,argb);return;}
    memcpy(native_stock, argb, sizeof(native_stock));
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
    if(VB_STREAM_SHADING && s_render && s_local_pixels && vb_viewport_active() && vb_viewport_can_stream_rows())
        return vb_viewport_render_local_pixels(vb_vip_display_fb()&1,eye,width,argb,
            view_levels,view_worlds,view_sources,s_render,s_context);
    uint32_t* stock=s_render ? view_stock:argb;
    if (!vb_viewport_active() || !vb_viewport_render(vb_vip_display_fb() & 1,
        eye, width, stock, view_levels, view_worlds, view_sources)) return 0;
    if (s_render) {
        memcpy(argb, stock, width * VB_RENDER_HEIGHT * sizeof(*argb));
        const VbRenderFrame frame = {vb_vip_frame_seq(), eye != 0, view_stock,
            view_levels, view_worlds, view_sources, width, VB_RENDER_HEIGHT};
        s_render(&frame, argb, s_context);
    }
    return 1;
}
const VbSourceTexel* vb_renderer_present_sources(int eye, int width) {
    return width == VB_RENDER_WIDTH ? vb_vip_source_buffer(eye) : view_sources;
}
