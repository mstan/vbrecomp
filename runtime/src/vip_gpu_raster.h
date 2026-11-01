#ifndef VB_VIP_GPU_RASTER_H
#define VB_VIP_GPU_RASTER_H
#include "vip_raster.h"
#include <memory>
struct SDL_Window;
struct SDL_Renderer;
struct VbVipGpuFrame {
    VbVipRasterBank banks[28]{};
    const VbVipRasterJob* jobs[2]{};
    unsigned counts[2]{};
    const uint8_t* bitmaps[2]{};
    uint64_t bitmap_revision[2]{};
    uint32_t colors[2][96][4]{};
    void stock_colors(int eye); /* Same active display LUT as CPU framebuffer decode. */
};
/* Owns an independent GL context. Never changes SDL renderer GL/cache state.
 * Sources/jobs are consumed synchronously by upload; GPU work owns resident
 * copies. The single GPU tile is at most2048², with a fixed12x7 native grid. */
struct VbVipGpuRasterCosts {
    uint64_t frames{},upload_bytes{},last_upload_bytes{},max_upload_bytes{};
    uint64_t gpu_source_upload_nanoseconds{},gpu_raster_compose_nanoseconds{};
    uint64_t gpu_nanoseconds{},gpu_max_nanoseconds{},timed_frames{},timer_pending{},timer_skipped{},init_failures{};
    unsigned requested_scale{},effective_scale{};bool timer_available{};
};
class VbVipGpuRaster {
    struct Impl;
    std::unique_ptr<Impl> impl_,legacy_;
    int legacy_timing_=-1;
    SDL_Window* attempted_window_{};bool attempted_{};uint64_t init_failures_{};
public:
    VbVipGpuRaster();~VbVipGpuRaster();
    bool initialize(SDL_Window*);
    bool present_current(SDL_Window*,int slot,bool stereo,unsigned scale,int width,int height,bool vsync=true);
    bool present(const VbVipGpuFrame&,bool stereo,unsigned scale,int width,int height,bool vsync=true);
    /* Existing native readback/debug contract: bounded32x32 temporary only. */
    bool readback_native(const VbVipGpuFrame&,int eye,uint32_t* native_argb);
    /* Opt-in finite profiling only; borrows a verified SDL OpenGL context. */
    void legacy_begin(SDL_Window*,SDL_Renderer*);
    void legacy_source(uint64_t bytes);
    void legacy_end(SDL_Renderer*);
    VbVipGpuRasterCosts legacy_costs();
    VbVipGpuRasterCosts costs(); /* Asynchronous GPU samples; never waits for unfinished queries. */
    void reset();
};
#endif
