#include "renderer.h"
#include "viewport.h"
#include "vip.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static unsigned legacy_calls,fused_calls,callback_calls;
static uint32_t* viewport_destination;
static uint16_t worlds[VB_VIEWPORT_MAX_WIDTH*224];
static VbSourceTexel sources[VB_VIEWPORT_MAX_WIDTH*224];
static uint32_t output[VB_VIEWPORT_MAX_WIDTH*224];
static uint32_t pixel(unsigned i,int eye){return 0xff000000u|((i*37u+eye*61u)&0xffffffu);}
uint32_t vb_vip_frame_seq(void){return 47;}
int vb_vip_display_fb(void){return 1;}
const uint16_t* vb_vip_attr_buffer(int eye){(void)eye;return worlds;}
const VbSourceTexel* vb_vip_source_buffer(int eye){(void)eye;return sources;}
void vb_vip_render_framebuffer(int eye,uint32_t* out){++legacy_calls;for(unsigned i=0;i<VB_RENDER_PIXELS;++i)out[i]=pixel(i,eye);}
void vb_vip_render_framebuffer_levels(int eye,uint32_t* out,uint8_t* levels){++fused_calls;for(unsigned i=0;i<VB_RENDER_PIXELS;++i){out[i]=pixel(i,eye);levels[i]=i&3;}}
int vb_viewport_width(void){return 512;}
int vb_viewport_active(void){return 1;}
void vb_viewport_reset(void){}
int vb_viewport_render(int slot,int eye,int width,uint32_t* out,uint8_t* levels,uint16_t* ws,VbSourceTexel* ss){
    assert(slot==1);viewport_destination=out;
    for(unsigned i=0;i<(unsigned)width*224;++i){out[i]=pixel(i,eye);levels[i]=i&3;ws[i]=i%33;ss[i].tile_hash=i;}
    return 1;
}
static void render(const VbRenderFrame* frame,uint32_t* out,void* context){
    assert(context==&callback_calls && frame->frame_seq==47 && frame->height==224);
    assert(frame->stock_argb!=out && frame->worlds && frame->sources);
    if(frame->width==384)assert(frame->worlds==worlds && frame->sources==sources);
    unsigned n=(unsigned)frame->width*frame->height;
    for(unsigned i=0;i<n;++i){
        assert(out[i]==pixel(i,frame->eye) && frame->stock_argb[i]==out[i]);
        assert(frame->levels[i]==(i&3));
        if(frame->width!=384)assert(frame->worlds[i]==i%33 && frame->sources[i].tile_hash==i);
        // Read arbitrary stock pixels after earlier output pixels changed.
        out[i]=frame->stock_argb[n-1-i]^0x0000ff00u;
    }
    for(unsigned i=0;i<n;++i)assert(frame->stock_argb[i]==pixel(i,frame->eye));
    ++callback_calls;
}
int main(void){
    vb_renderer_track_texels();
    for(int eye=0;eye<2;++eye){
        vb_renderer_present(eye,output);
        for(unsigned i=0;i<VB_RENDER_PIXELS;++i)assert(output[i]==pixel(i,eye));
        assert(vb_renderer_present_viewport(eye,512,output));assert(viewport_destination==output);
        for(unsigned i=0;i<512*224;++i)assert(output[i]==pixel(i,eye));
    }
    assert(legacy_calls==2 && !fused_calls);
    assert(vb_renderer_register("contract",render,&callback_calls));
    for(int eye=0;eye<2;++eye){
        for(unsigned width=384;width<=512;width+=128){
            assert(vb_renderer_present_viewport(eye,width,output));
            for(unsigned i=0;i<width*224;++i)assert(output[i]==(pixel(width*224-1-i,eye)^0x0000ff00u));
            if(width==512)assert(viewport_destination!=output);
        }
    }
    assert(legacy_calls==2 && fused_calls==2 && callback_calls==4);
    vb_renderer_reset();vb_renderer_present(0,output);assert(legacy_calls==3);
    puts("PASS: native fused decode, callback read-only stock/prefill and direct no-callback viewport output");
}
