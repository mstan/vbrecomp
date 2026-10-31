/* Adapted hidden SDL/native readback fixture. No timers/collectors: compare
 * actual GPU pixels to the existing portable VIP jobs at native density. */
#include "../src/vip_gpu_raster.h"
#include "viewport.h"
#include "red_lut.h"
#include <SDL.h>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
extern "C" {
int vb_vip_display_fb(void){return 0;}
uint32_t vb_vip_frame_seq(void){return 1;}
uint32_t vb_vip_raster_bitmap_rows(int){return 0;}
const uint8_t* vb_vip_raster_bitmap(int,int,uint64_t*){return nullptr;}
void vb_vip_column_argb(int eye,uint32_t colors[96][4]) {
    for(unsigned x=0;x<96;++x)for(unsigned v=0;v<4;++v)colors[x][v]=0xff000000u|((x*13+v*37+eye*17)&255)<<16;
}
}
static unsigned seed=0x27f1aa31;
static unsigned random_value(){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int main(int argc,char** argv) {
    const bool palette_only=argc==2 && !std::strcmp(argv[1],"--palette-only");assert(argc==1 || palette_only);
    assert(SDL_Init(SDL_INIT_VIDEO)==0);
    SDL_Window* window=SDL_CreateWindow("Hidden VIP raster readback",0,0,768,448,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);assert(window);
    VbVipGpuRaster gpu;
    SDL_Window* plain=SDL_CreateWindow("Hidden portable fallback",0,0,32,32,SDL_WINDOW_HIDDEN);assert(plain);
    assert(!gpu.initialize(plain));assert(!gpu.initialize(plain));assert(gpu.costs().init_failures==1);gpu.reset();SDL_DestroyWindow(plain);
    assert(gpu.initialize(window));
    std::vector<uint16_t> dram(65536),chars(16384);uint16_t spt[4]={127,255,511,1023},suppress[1024]{};
    uint8_t gplt[4][4],jplt[4][4];VbVipGpuFrame frame{};
    std::vector<VbVipRasterJob> jobs[2];for(auto& j:jobs)j.resize(VB_VIP_RASTER_JOB_LIMIT);
    std::vector<uint8_t> levels(384*224),bitmap(64*384);
    std::vector<uint32_t> actual(384*224),reference(384*224);
    std::vector<uint8_t> reference_levels(384*224);
    std::vector<uint16_t> reference_worlds(384*224);std::vector<VbSourceTexel> reference_sources(384*224);
    vb_viewport_track();
    for(unsigned scene=0;scene<(palette_only?1u:6u);++scene) {
        for(auto& v:dram)v=(uint16_t)random_value();for(auto& v:chars)v=(uint16_t)random_value();
        for(auto& v:bitmap)v=(uint8_t)random_value();
        for(int p=0;p<4;++p)for(int i=0;i<4;++i){gplt[p][i]=random_value()&3;jplt[p][i]=random_value()&3;}
        uint16_t* worlds=dram.data()+0x1d800/2;
        for(int w=31;w>=24;--w) {
            uint16_t* v=worlds+w*16;v[0]=(uint16_t)(0xc000|((scene+w)%4)<<12|((scene&1)?128:0)|(w&15));
            v[1]=(uint16_t)((w-28)*7);v[2]=scene*3;v[3]=0;v[4]=scene*35;v[5]=scene*7;v[6]=scene*19;v[7]=383;v[8]=223;v[9]=0x1000;v[10]=scene*29;
        }
        worlds[23*16]=64;
        for(int row=0;row<28;++row) {
            vb_viewport_capture(0,row,dram.data(),chars.data(),spt,gplt,jplt,scene&3,suppress);
            assert(vb_viewport_raster_bank(0,row,frame.banks+row));
        }
        uint32_t bitmap_rows=scene==5?0x0fffffffu:0;
        for(int eye=0;eye<2;++eye) {
            assert(vb_vip_raster_prepare(frame.banks,eye,bitmap_rows,jobs[eye].data(),VB_VIP_RASTER_JOB_LIMIT,&frame.counts[eye]));
            frame.jobs[eye]=jobs[eye].data();frame.bitmaps[eye]=bitmap.data();frame.bitmap_revision[eye]=0;
            frame.stock_colors(eye);
            uint32_t raw[96][4];vb_vip_column_argb(eye,raw);
            for(unsigned x=0;x<96;++x)for(unsigned v=0;v<4;++v)assert(frame.colors[eye][x][v]==vb_red_lut_map((raw[x][v]>>16)&255));
            if(vb_red_lut_active_kind()==VB_SCREEN_LED_WARM)assert(vb_red_lut_map(255)==0xffff1900u);
            assert(vb_vip_raster_cpu(frame.banks,frame.jobs[eye],frame.counts[eye],eye,bitmap.data(),levels.data()));
            if(scene!=5){
                assert(vb_viewport_render(0,eye,384,reference.data(),reference_levels.data(),reference_worlds.data(),reference_sources.data()));
                for(unsigned at=0;at<384*224;++at)if(levels[at]!=reference_levels[at]){std::fprintf(stderr,"portable mismatch scene=%u eye=%d x=%u y=%u expected=%u got=%u\n",scene,eye,at%384,at/384,reference_levels[at],levels[at]);return 1;}
            }
            assert(gpu.readback_native(frame,eye,actual.data()));
            for(unsigned y=0;y<224;++y)for(unsigned x=0;x<384;++x)if(actual[y*384+x]!=frame.colors[eye][x/4][levels[y*384+x]&3]){std::fprintf(stderr,"GPU mismatch scene=%u eye=%d x=%u y=%u expected=%08x got=%08x\n",scene,eye,x,y,frame.colors[eye][x/4][levels[y*384+x]&3],actual[y*384+x]);return 1;}
        }
        // Full requested densities use the same frame/jobs and bounded tile;
        // no CPU enhanced framebuffer is created or uploaded.
        assert(gpu.present(frame,true,palette_only?1:scene==0?16:scene==1?32:scene==2?64:1,768,448,false));
    }
    auto costs=gpu.costs();assert(costs.frames==(palette_only?1u:6u));
    std::printf("VIP_GPU_COMPONENT frames=%llu upload_bytes=%llu timed_frames=%llu gpu_ns=%llu timer_pending=%llu\n",(unsigned long long)costs.frames,(unsigned long long)costs.upload_bytes,(unsigned long long)costs.timed_frames,(unsigned long long)costs.gpu_nanoseconds,(unsigned long long)costs.timer_pending);
    gpu.reset();SDL_DestroyWindow(window);SDL_Quit();
    std::puts(palette_only?"PASS: active display LUT matches CPU/native GPU pixels in both eyes":"PASS: native GPU/portable VIP job pixels, normal/HBias/affine/OBJ/bitmap, both eyes; bounded16/32/64x presentation");
}
