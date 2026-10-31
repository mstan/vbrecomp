/* Private raster template: include once for normal output and once for sparse
 * thumbnails. Compile-time selection keeps the retained normal-frame body free
 * of row masks/branches; both paths share raster, layout and shader semantics.
 * Intentionally no include guard. */
static int VB_VIEWPORT_RENDER_NAME(int slot, int eye, int width, uint32_t* argb,
                       uint8_t* levels, uint16_t* worlds, VbSourceTexel* sources,
                       VbRenderCallback shade,void* context
#if VB_VIEWPORT_RENDER_SAMPLED
                       ,unsigned sample_height
#endif
                       ) {
    if (slot < 0 || slot > 1 || width < 384 || width > VB_VIEWPORT_MAX_WIDTH
        || (width & 1) || !argb || !levels || !worlds || !sources) return 0;
    for (int row = 0; row < 28; ++row) if (!blocks[slot][row]->valid) return 0;
#if VB_VIEWPORT_RENDER_SAMPLED
    uint8_t wanted[224]={0};
    for(unsigned y=0;y<sample_height;++y)wanted[y*224/sample_height]=1;
#endif
    if(!shade) {
        memset(worlds, 0, width * 224 * sizeof(*worlds));
        memset(sources, 0, width * 224 * sizeof(*sources));
    }
    uint32_t palette[96][4];
    int origins[28];
    vb_vip_column_argb(eye, palette);
    const uint32_t frame_seq=shade?vb_vip_frame_seq():0;
    for (int row = 0; row < 28; ++row) {
        if(shade) {
#if VB_VIEWPORT_RENDER_SAMPLED
            for(int y=row*8;y<row*8+8;++y)if(wanted[y]) {
                memset(worlds+y*width,0,width*sizeof(*worlds));
                memset(sources+y*width,0,width*sizeof(*sources));
            }
#else
            const unsigned at=(unsigned)row*8*width;
            memset(worlds+at,0,8*width*sizeof(*worlds));
            memset(sources+at,0,8*width*sizeof(*sources));
#endif
        }
        const ViewBlock* b = blocks[slot][row];
        VbViewportScene scene = scene_of(b);
        int kind = classify_scene ? classify_scene(&scene, policy_context) : 0;
        VbViewportLayout layout = {0};
        if (kind && width > 384) arrange_scene(&scene, kind, width, &layout, policy_context);
        ViewTarget target = {0};
        target.block = b; target.layout = &layout; target.width = width;
        target.margin = (width - 384) / 2 + layout.camera_shift_x; target.eye = eye != 0;
        origins[row] = target.margin;
        target.levels = levels; target.worlds = worlds; target.sources = sources;
#if VB_VIEWPORT_RENDER_SAMPLED
        for(int y=row*8;y<row*8+8;++y)if(wanted[y])memset(levels+y*width,b->backdrop,width);
#else
        memset(levels + row * 8 * width, b->backdrop, 8 * width);
#endif
        int group = 3;
        for (int world = 31; world >= 0; --world) {
            const uint16_t* w = scene.worlds + world * 16;
            if (w[0] & 64) break;
            target.world = world; target.kind = (w[0] >> 12) & 3;
            target.map = target.kind == 3 ? 255 : w[0] & 15;
            if (w[0] & (eye ? 0x4000 : 0x8000)) {
                for (int y = row * 8; y < row * 8 + 8; ++y) {

#if VB_VIEWPORT_RENDER_SAMPLED
                if(!wanted[y])continue;
#endif
                    target.y = y;
                    if (target.kind == 3) objects(&target, group, width > 384 && !kind);
                    else background(&target, w, width > 384 && !kind);
                }
            }
            if (target.kind == 3 && group) --group;
            if (kind && width > 384) for (int y = row * 8; y < row * 8 + 8; ++y) {

#if VB_VIEWPORT_RENDER_SAMPLED
                if(!wanted[y])continue;
#endif
                target.y = y; extra_sprites(&target);
            }
        }
        if(shade) {
            uint32_t stock[VB_VIEWPORT_MAX_WIDTH];
            for(int y=row*8;y<row*8+8;++y) {

#if VB_VIEWPORT_RENDER_SAMPLED
                if(!wanted[y])continue;
#endif
                const unsigned at=(unsigned)y*width;
                for(int x=0;x<width;++x) {
                    int native_x=x-origins[row];
                    unsigned group=native_x<0?0:native_x>383?95:(unsigned)native_x/4;
                    stock[x]=palette[group][levels[at+x]&3];
                }
                memcpy(argb+at,stock,width*sizeof(*stock));
                const VbRenderFrame frame={frame_seq,eye!=0,stock,levels+at,worlds+at,sources+at,width,1};
                shade(&frame,argb+at,context);
            }
        }
    }
    if(!shade)for (int y = 0; y < 224; ++y) for (int x = 0; x < width; ++x) {
        /* Preserve native CT alignment in the center. Extra columns inherit
         * the nearest native edge's brightness; no hardware CT exists there. */
        int native_x = x - origins[y / 8];
        unsigned group = native_x < 0 ? 0 : native_x > 383 ? 95 : (unsigned)native_x / 4;
        unsigned i = y * width + x;
        argb[i] = palette[group][levels[i] & 3];
    }
    return 1;
}
