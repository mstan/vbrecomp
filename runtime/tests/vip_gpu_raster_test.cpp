/* Adapted hidden SDL/native readback fixture. No timers/collectors: compare
 * actual GPU pixels to the existing portable VIP jobs at native density. */
#include "../src/vip_gpu_raster.h"
#include "viewport.h"
#include "red_lut.h"
#include <SDL.h>
#include <SDL_opengl.h>
#include <cmath>
#include <cassert>
#include <algorithm>
#include <cstdlib>
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
static const char* missing_proc;
extern "C" void* vb_vip_test_gl_proc(const char* name) {
    return missing_proc && !std::strcmp(name,missing_proc)?nullptr:SDL_GL_GetProcAddress(name);
}
static const VbVipGpuFrame* checked_frame;
static unsigned checked_scale;
static bool checked_affine;
extern "C" void vb_vip_test_presented(int width,int height) {
    if(!checked_frame)return;
    using BindFn=void(APIENTRY*)(GLenum,GLuint);using ReadBufferFn=void(APIENTRY*)(GLenum);
    using ReadFn=void(APIENTRY*)(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*);
    auto bind=reinterpret_cast<BindFn>(SDL_GL_GetProcAddress("glBindFramebuffer"));
    auto buffer=reinterpret_cast<ReadBufferFn>(SDL_GL_GetProcAddress("glReadBuffer"));
    auto read=reinterpret_cast<ReadFn>(SDL_GL_GetProcAddress("glReadPixels"));assert(bind&&buffer&&read);
    // Test output stays500x585, independently of internal density.
    std::vector<uint32_t> pixels(size_t(width)*height);bind(GL_READ_FRAMEBUFFER,0);buffer(GL_BACK);
    read(0,0,width,height,GL_BGRA,GL_UNSIGNED_INT_8_8_8_8_REV,pixels.data());
    const double fit=std::min(double(width)/384,double(height)/448);
    int dw=int(384*fit),dh=int(448*fit),left=(width-dw)/2,top=(height-dh)/2;
    assert(dw==500 && (dh&1));
    for(int y=0;y<height;++y)for(int x=0;x<width;++x) {
        uint32_t actual=pixels[size_t(height-1-y)*width+x],expected=0xff000000u;bool matched=false;
        if(x>=left&&x<left+dw&&y>=top&&y<top+dh) {
            uint64_t ux=uint64_t(2*(x-left)+1)*384*checked_scale,uy=uint64_t(2*(y-top)+1)*448*checked_scale;
            unsigned ex=ux/(2*dw),ey=uy/(2*dh);
            // At an exact source pixel edge, both neighboring centers are
            // equally near. Accept either tie choice, but check every pixel.
            unsigned xn=(ex && ux%(2*dw)==0)?2:1,yn=(ey && uy%(2*dh)==0)?2:1;
            for(unsigned iy=0;iy<yn;++iy)for(unsigned ix=0;ix<xn;++ix) {
                unsigned hx=ex-ix,hy=ey-iy,nx=hx/checked_scale,ny=(hy/checked_scale)%224,eye=hy/(224*checked_scale),level;
                if(checked_affine) {
                    // Adversarial dx=dy=-1 starts on an exact fixed-point boundary.
                    int fixed=int(std::floor(-double(hx)/checked_scale));
                    unsigned sample=(unsigned(fixed)&((512u<<9)-1))>>9;
                    const uint16_t* chars=checked_frame->banks[ny/8].input.chars;
                    level=(chars[8+(sample&7)]>>((sample&7)*2))&3;
                } else level=(checked_frame->bitmaps[eye][nx*64+ny/4]>>((ny&3)*2))&3;
                expected=checked_frame->colors[eye][nx/4][level];matched|=actual==expected;
            }
        } else matched=actual==expected;
        if(!matched){std::fprintf(stderr,"global resize/phase mismatch scale=%u affine=%d x=%d y=%d expected=%08x got=%08x\n",checked_scale,checked_affine,x,y,expected,actual);std::abort();}
    }
}
static unsigned seed=0x27f1aa31;
static unsigned random_value(){seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed;}
int main(int argc,char** argv) {
    const bool palette_only=argc==2 && !std::strcmp(argv[1],"--palette-only");
    const bool corrections=argc==2 && !std::strcmp(argv[1],"--corrections-only");
    const bool legacy=argc==2 && !std::strcmp(argv[1],"--legacy-timing-only");assert(argc==1 || palette_only || corrections || legacy);
    assert(SDL_Init(SDL_INIT_VIDEO)==0);
    SDL_Window* window=SDL_CreateWindow("Hidden VIP raster readback",0,0,768,448,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);assert(window);
    VbVipGpuRaster gpu;
    SDL_Window* plain=SDL_CreateWindow("Hidden portable fallback",0,0,32,32,SDL_WINDOW_HIDDEN);assert(plain);
    assert(!gpu.initialize(plain));assert(!gpu.initialize(plain));assert(gpu.costs().init_failures==1);gpu.reset();SDL_DestroyWindow(plain);
    if(legacy) {
        SDL_SetHint(SDL_HINT_RENDER_DRIVER,"opengl");SDL_Renderer* renderer=SDL_CreateRenderer(window,-1,SDL_RENDERER_ACCELERATED);assert(renderer);
        SDL_Texture* texture=SDL_CreateTexture(renderer,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,384,224);assert(texture);
        std::vector<uint32_t> stock(384*224,0xffdd1100);auto context=SDL_GL_GetCurrentContext();assert(context);
        for(unsigned i=0;i<5;++i){gpu.legacy_begin(window,renderer);assert(SDL_UpdateTexture(texture,nullptr,stock.data(),384*4)==0);gpu.legacy_source(stock.size()*4);SDL_RenderClear(renderer);SDL_RenderCopy(renderer,texture,nullptr,nullptr);gpu.legacy_end(renderer);SDL_RenderPresent(renderer);}
        uint32_t pixel;SDL_Rect one{0,0,1,1};assert(SDL_RenderReadPixels(renderer,&one,SDL_PIXELFORMAT_ARGB8888,&pixel,4)==0);
        auto times=gpu.legacy_costs();SDL_RendererInfo info{};SDL_GetRendererInfo(renderer,&info);std::printf("VIP_LEGACY_DIAGNOSTIC renderer=%s available=%u timed=%llu pending=%llu skipped=%llu samewindow=%u\n",info.name,(unsigned)times.timer_available,(unsigned long long)times.timed_frames,(unsigned long long)times.timer_pending,(unsigned long long)times.timer_skipped,(unsigned)(SDL_GL_GetCurrentWindow()==window));std::fflush(stdout);assert(times.timer_available && times.timed_frames>0 && times.timed_frames+times.timer_pending+times.timer_skipped==5);assert(times.gpu_nanoseconds==times.gpu_source_upload_nanoseconds+times.gpu_raster_compose_nanoseconds);assert(gpu.costs().frames==0 && !gpu.costs().init_failures);
        std::printf("VIP_LEGACY_GPU_PASSES source_upload_ns=%llu compose_ns=%llu frames=%llu\n",(unsigned long long)times.gpu_source_upload_nanoseconds,(unsigned long long)times.gpu_raster_compose_nanoseconds,(unsigned long long)times.timed_frames);
        gpu.reset();assert(SDL_GL_GetCurrentContext()==context);assert(SDL_RenderClear(renderer)==0);SDL_RenderPresent(renderer);SDL_DestroyTexture(texture);SDL_DestroyRenderer(renderer);SDL_DestroyWindow(window);SDL_Quit();std::puts("PASS: borrowed legacy SDL pass timestamps; renderer context retained, no raster initialization");return 0;
    }
    if(corrections) {
        missing_proc="glTexImage3D";assert(!gpu.initialize(window));assert(gpu.costs().init_failures==1);
        assert(!gpu.initialize(window));gpu.reset();missing_proc=nullptr;
    }
    assert(gpu.initialize(window));
    std::vector<uint16_t> dram(65536),chars(16384);uint16_t spt[4]={127,255,511,1023},suppress[1024]{};
    uint8_t gplt[4][4],jplt[4][4];VbVipGpuFrame frame{};
    std::vector<VbVipRasterJob> jobs[2];for(auto& j:jobs)j.resize(VB_VIP_RASTER_JOB_LIMIT);
    std::vector<uint8_t> levels(384*224),bitmap(64*384);
    std::vector<uint32_t> actual(384*224),reference(384*224);
    std::vector<uint8_t> reference_levels(384*224);
    std::vector<uint16_t> reference_worlds(384*224);std::vector<VbSourceTexel> reference_sources(384*224);
    vb_viewport_track();
    for(unsigned scene=0;scene<((palette_only||corrections)?1u:6u);++scene) {
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
        assert(gpu.present(frame,true,(palette_only||corrections)?1:scene==0?16:scene==1?32:scene==2?64:1,768,448,false));
    }
    if(corrections) {
        SDL_SetWindowSize(window,500,585);
        int width=0,height=0;SDL_GL_GetDrawableSize(window,&width,&height);assert(width==500 && height==585);
        for(int eye=0;eye<2;++eye) {
            assert(vb_vip_raster_prepare(frame.banks,eye,0x0fffffffu,jobs[eye].data(),VB_VIP_RASTER_JOB_LIMIT,&frame.counts[eye]));
            frame.jobs[eye]=jobs[eye].data();frame.bitmaps[eye]=bitmap.data();frame.bitmap_revision[eye]=0;
        }
        checked_frame=&frame;checked_scale=1;checked_affine=false;
        assert(gpu.present(frame,true,1,width,height,false));
        // Same odd stereo output, but true affine16x source sampling. All
        // CPU reference/readback storage remains output/native sized.
        std::fill(dram.begin(),dram.end(),0);std::fill(chars.begin(),chars.end(),0xaaaa);
        for(unsigned i=0;i<4096;++i)dram[i]=1;
        chars[8]=(chars[8]&~3u)|1;chars[15]|=3u<<14;
        uint16_t* w=dram.data()+0x1d800/2+31*16;w[0]=0xe000;w[7]=383;w[8]=223;w[9]=0x4000;
        dram[0x1d800/2+30*16]=64;
        for(unsigned y=0;y<224;++y){dram[0x4000+y*8+3]=uint16_t(-1);dram[0x4000+y*8+4]=uint16_t(-1);}
        for(unsigned p=0;p<4;++p)for(unsigned i=0;i<4;++i)gplt[p][i]=jplt[p][i]=i;
        for(int row=0;row<28;++row){vb_viewport_capture(0,row,dram.data(),chars.data(),spt,gplt,jplt,0,suppress);assert(vb_viewport_raster_bank(0,row,frame.banks+row));}
        for(int eye=0;eye<2;++eye){assert(vb_vip_raster_prepare(frame.banks,eye,0,jobs[eye].data(),VB_VIP_RASTER_JOB_LIMIT,&frame.counts[eye]));frame.jobs[eye]=jobs[eye].data();frame.bitmaps[eye]=nullptr;}
        checked_scale=16;checked_affine=true;assert(gpu.present(frame,true,16,width,height,false));checked_frame=nullptr;
    }
    auto costs=gpu.costs();assert(costs.frames==(corrections?3u:palette_only?1u:6u));
    assert(costs.gpu_source_upload_nanoseconds+costs.gpu_raster_compose_nanoseconds==costs.gpu_nanoseconds);
    std::printf("VIP_GPU_PASSES source_upload_ns=%llu raster_compose_ns=%llu\n",(unsigned long long)costs.gpu_source_upload_nanoseconds,(unsigned long long)costs.gpu_raster_compose_nanoseconds);
    std::printf("VIP_GPU_COMPONENT frames=%llu upload_bytes=%llu timed_frames=%llu gpu_ns=%llu timer_pending=%llu\n",(unsigned long long)costs.frames,(unsigned long long)costs.upload_bytes,(unsigned long long)costs.timed_frames,(unsigned long long)costs.gpu_nanoseconds,(unsigned long long)costs.timer_pending);
    gpu.reset();SDL_DestroyWindow(window);SDL_Quit();
    std::puts(corrections?"PASS: partial API fallback, odd global stereo resize, signed affine16x floor phase, exact native pixels":palette_only?"PASS: active display LUT matches CPU/native GPU pixels in both eyes":"PASS: native GPU/portable VIP job pixels, normal/HBias/affine/OBJ/bitmap, both eyes; bounded16/32/64x presentation");
}
