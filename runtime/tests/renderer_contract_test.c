#include "renderer.h"
#include "viewport.h"
#include "vip.h"
#include "../src/source_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef VB_STREAM_SHADING
#define VB_STREAM_SHADING 1
#endif

static unsigned legacy_calls,fused_calls,callback_calls;
static unsigned local_calls,streamed_calls;
static unsigned viewport_epoch;
static int viewport_local;
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
void vb_viewport_reset(void){viewport_local=0;}
int vb_viewport_can_stream_rows(void){return viewport_local;}
int vb_viewport_render(int slot,int eye,int width,uint32_t* out,uint8_t* levels,uint16_t* ws,VbSourceTexel* ss){
    assert(slot==1);viewport_destination=out;
    for(unsigned i=0;i<(unsigned)width*224;++i){unsigned n=i+viewport_epoch;out[i]=pixel(n,eye);levels[i]=n&3;ws[i]=n%33;ss[i].tile_hash=n;}
    return 1;
}
int vb_viewport_render_local_pixels(int slot,int eye,int width,uint32_t* out,uint8_t* levels,
                                    uint16_t* ws,VbSourceTexel* ss,VbRenderCallback callback,void* context) {
    assert(slot==1 && viewport_local);++streamed_calls;viewport_destination=out;
    uint32_t stock[VB_VIEWPORT_MAX_WIDTH];
    for(unsigned y=0;y<224;++y) {
        unsigned at=y*width;
        for(int x=0;x<width;++x) {
            unsigned i=at+x,n=i+viewport_epoch;out[i]=stock[x]=pixel(n,eye);levels[i]=n&3;ws[i]=n%33;ss[i].tile_hash=n;
        }
        VbRenderFrame row={47,eye,stock,levels+at,ws+at,ss+at,width,1};
        callback(&row,out+at,context);
    }
    return 1;
}
int vb_viewport_render_packed_sources(int slot,int eye,int width,uint32_t* out,uint8_t* levels,
    uint16_t* ws,VbSourceTexel* ss,VbPackedSourceTexel* packed,VbSourceTexel* full,VbRenderCallback callback,void* context) {
    assert(slot==1 && viewport_local);++streamed_calls;viewport_destination=out;
    uint32_t stock[VB_VIEWPORT_MAX_WIDTH];
    for(unsigned y=0;y<224;++y) {
        unsigned at=(y%8)*width,screen=y*width;
        for(int x=0;x<width;++x){unsigned n=screen+x+viewport_epoch;out[screen+x]=stock[x]=pixel(n,eye);levels[at+x]=n&3;ws[at+x]=n%33;ss[at+x]=(VbSourceTexel){0};ss[at+x].tile_hash=n;}
        VbRenderFrame row={47,eye,stock,levels+at,ws+at,ss+at,width,1};callback(&row,out+screen,context);
        if(full)memcpy(full+screen,ss+at,width*sizeof(*full));
        else for(int x=0;x<width;++x)packed[screen+x]=vb_source_pack(ss[at+x]);
    }
    return 1;
}
static uint32_t local_value(unsigned i,int eye){return pixel(i,eye)^((i+(i%33)*17+(i&3))&0xffffff);}
static void render_local(const VbRenderFrame* frame,uint32_t* out,void* context) {
    assert(context==&local_calls && frame->stock_argb!=out);
    assert(frame->frame_seq==47 && (frame->height==1 || frame->height==224));
    for(unsigned i=0;i<(unsigned)frame->width*frame->height;++i) {
        unsigned n=frame->sources[i].tile_hash;
        assert(frame->stock_argb[i]==pixel(n,frame->eye) && out[i]==frame->stock_argb[i]);
        assert(frame->worlds[i]==n%33 && frame->levels[i]==(n&3));
        out[i]=local_value(n,frame->eye);
        assert(frame->stock_argb[i]==pixel(n,frame->eye));
    }
    ++local_calls;
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
int main(int argc,char** argv){
    (void)argv;
    if(argc>1) {
        vb_renderer_track_texels();viewport_local=1;
        assert(vb_renderer_register_local_pixels("compact-first",render_local,&local_calls));
        assert(vb_renderer_present_viewport(0,512,output));
        unsigned calls=local_calls;viewport_epoch=1000;
        const VbSourceTexel* held=vb_renderer_present_sources(0,512);
        assert(local_calls==calls);
        for(unsigned i=0;i<512*224;++i)assert(held[i].tile_hash==i);
        assert(vb_renderer_present_viewport(1,398,output));
        assert(held==vb_renderer_present_sources(1,398));
        for(unsigned i=0;i<398*224;++i)assert(held[i].tile_hash==i+1000);
        assert(held[398*224+5].tile_hash==398*224+5);
        puts("PASS: delayed source materialization preserves last frame without callback replay; published pointer and old tails stay live");return 0;
    }
    vb_renderer_track_texels();
    for(int eye=0;eye<2;++eye){
        vb_renderer_present(eye,output);
        for(unsigned i=0;i<VB_RENDER_PIXELS;++i)assert(output[i]==pixel(i,eye));
        assert(vb_renderer_present_viewport(eye,512,output));assert(viewport_destination==output);
        for(unsigned i=0;i<512*224;++i)assert(output[i]==pixel(i,eye));
    }
    assert(legacy_calls==2 && !fused_calls);
    assert(vb_renderer_register("contract",render,&callback_calls));
    assert(!vb_renderer_register_local_pixels("collision",render_local,&local_calls));
    for(int eye=0;eye<2;++eye){
        for(unsigned width=384;width<=512;width+=128){
            assert(vb_renderer_present_viewport(eye,width,output));
            for(unsigned i=0;i<width*224;++i)assert(output[i]==(pixel(width*224-1-i,eye)^0x0000ff00u));
            if(width==512)assert(viewport_destination!=output);
        }
    }
    assert(legacy_calls==2 && fused_calls==2 && callback_calls==4);
    vb_renderer_reset();vb_renderer_present(0,output);assert(legacy_calls==3);
    for(unsigned i=0;i<VB_RENDER_PIXELS;++i){worlds[i]=i%33;sources[i].tile_hash=i;}
    assert(vb_renderer_register_local_pixels("local",render_local,&local_calls));
    for(int eye=0;eye<2;++eye) {
        local_calls=0;vb_renderer_present(eye,output);assert(local_calls==(VB_STREAM_SHADING?224u:1u));
        for(unsigned i=0;i<VB_RENDER_PIXELS;++i)assert(output[i]==local_value(i,eye));
        viewport_local=0;local_calls=0;
        assert(vb_renderer_present_viewport(eye,512,output));assert(local_calls==1 && !streamed_calls);
        for(unsigned i=0;i<512*224;++i)assert(output[i]==local_value(i,eye));
    }
    viewport_local=1;
    for(int eye=0;eye<2;++eye) {
        local_calls=0;assert(vb_renderer_present_viewport(eye,512,output));assert(local_calls==(VB_STREAM_SHADING?224u:1u));
        for(unsigned i=0;i<512*224;++i)assert(output[i]==local_value(i,eye));
    }
    assert(streamed_calls==(VB_STREAM_SHADING?2u:0u));
    static uint8_t raw[VB_RENDER_PIXELS];
    for(unsigned i=0;i<VB_RENDER_PIXELS;++i){output[i]=pixel(i,0);raw[i]=i&3;}
    VbRenderFrame alias={47,0,output,raw,worlds,sources,384,224};
    assert(vb_renderer_apply_frame(&alias,output));
    for(unsigned i=0;i<VB_RENDER_PIXELS;++i)assert(output[i]==local_value(i,0));
    puts("PASS: native fused decode, callback read-only stock/prefill and direct no-callback viewport output");
    puts("PASS: explicitly local rows, independent viewport qualification, legacy fallback, alias input and reset/registration isolation");
}
