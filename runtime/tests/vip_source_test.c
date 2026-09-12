/* Exercise the real rasterizer with synthetic CHR; no game ROM required. */
#include <assert.h>
#include <stdio.h>
#include "../src/vip.c"
static int tracking=1;
int vb_renderer_tracks_texels(void) {return tracking;}
int vb_renderer_tracks_worlds(void) {return 1;}
uint32_t vb_capture_hash(const uint16_t ptr[8]) {
    const uint8_t* p=(const uint8_t*)ptr;uint32_t h=2166136261u;
    for(int i=0;i<16;++i)h=(h^p[i])*16777619u;
    return h;
}
static uint8_t left[0x6000],right[0x6000],baseline[0x6000],baseline_right[0x6000];
static void setup(unsigned mode,unsigned flips,int diagonal) {
    memset(s_vip_mem,0,sizeof(s_vip_mem));
    memset(s_source_fb,0,sizeof(s_source_fb));
    memset(s_attr_fb,0,sizeof(s_attr_fb));
    s_attr_on=1;s_drawing_fb=0;s_display_fb=1;s_bkcol=2;
    for(int p=0;p<4;++p)for(int v=0;v<4;++v)s_gplt_cache[p][v]=s_jplt_cache[p][v]=(uint8_t)v;
    uint16_t* chr=(uint16_t*)(s_vip_mem+0x78000);
    uint16_t* bgm=(uint16_t*)(s_vip_mem+0x20000);
    for(int y=0;y<8;++y)for(int x=0;x<8;++x)chr[8+y]|=((x+2*y)%4)<<(2*x);
    for(int i=0;i<4096;++i)bgm[4096+i]=1|flips;
    uint16_t* w=&bgm[(0x1d800+31*32)/2];
    w[0]=0xc001|(mode<<12);w[7]=15;w[8]=7;w[9]=0x3000;
    bgm[(0x1d800+30*32)/2]=0x40;
    for(int y=0;y<8;++y) {
        uint16_t* p=&bgm[0x3000+y*8];p[0]=0;p[2]=y*8;p[3]=512;p[4]=diagonal?512:0;
        if(mode==1){bgm[0x3000+y*2]=1;bgm[0x3000+y*2+1]=2;}
    }
    if(mode==3) {
        s_spt[3]=1;s_spt[2]=0;
        uint16_t* o=&bgm[(0x1e000+8)/2];o[0]=3;o[1]=0xc001;o[2]=0;o[3]=1|flips;
    }
}
static int viewport_scene(const VbViewportScene* scene, void* context) {
    (void)scene; (void)context; return 1;
}
static void viewport_layout(const VbViewportScene* scene, int kind, int width,
                            VbViewportLayout* layout, void* context) {
    (void)scene; (void)kind; (void)width; (void)layout; (void)context;
}
static uint8_t extra_marker = 73;
static void snapshot_extra(uint8_t* bytes, unsigned count) { memset(bytes,extra_marker,count); }
static int extend_background(int x,int y,int* source_x,int* source_y,void* context) {
    (void)context;*source_x=(x%8+8)%8;*source_y=y;return 1;
}
static void extended_layout(const VbViewportScene* scene,int kind,int width,
                            VbViewportLayout* layout,void* context) {
    (void)kind;(void)width;(void)context;
    assert(scene->extra_size==4 && scene->extra[0]==73 && scene->extra[3]==73);
    layout->worlds[31].outside=extend_background;
    static const uint16_t cells[1]={0x3001};
    for(int eye=0;eye<2;++eye)
        layout->sprites[layout->sprite_count++]=(VbViewportSprite){31,eye,430+eye*2,8,1,1,2048,cells,scene->chars};
}
static void viewport_tests(void) {
    enum { W = 796, N = W * 224 };
    static uint32_t colors[N];
    static uint8_t levels[N], saved[384 * 224];
    static uint16_t worlds[N];
    static VbSourceTexel sources[N];
    vb_viewport_track();
    assert(vb_viewport_register("test.viewport",32,9,0,viewport_scene,viewport_layout,0));
    assert(!vb_viewport_register("test.conflict",16,9,0,viewport_scene,viewport_layout,0));
    for(unsigned mode=0;mode<4;++mode)for(unsigned flip=0;flip<4;++flip)for(int diagonal=0;diagonal<2;++diagonal) {
        setup(mode,flip<<12,diagonal);
        uint16_t* bgm=(uint16_t*)(s_vip_mem+0x20000);
        uint16_t* w=&bgm[(0x1d800+31*32)/2];
        w[1]=(uint16_t)-12; w[7]=420; w[8]=7;
        for(int block=0;block<28;++block)vip_draw_block_into(block,left,right);
        s_display_fb=0;
        assert(vb_viewport_width()==796);
        for(int eye=0;eye<2;++eye) {
            const uint8_t* fb=eye?right:left;
            assert(vb_viewport_render(0,eye,384,colors,levels,worlds,sources));
            for(int y=0;y<224;++y)for(int x=0;x<384;++x) {
                unsigned i=y*384+x;
                unsigned raw=(fb[x*64+y/4]>>(2*(y%4)))&3;
                assert(levels[i]==raw);
                assert(!memcmp(&sources[i],&s_source_fb[0][eye][i],sizeof(VbSourceTexel)));
            }
            memcpy(saved,levels,sizeof(saved));
            assert(vb_viewport_render(0,eye,W,colors,levels,worlds,sources));
            for(int y=0;y<224;++y)for(int x=0;x<384;++x)
                assert(levels[y*W+x+(W-384)/2]==saved[y*384+x]);
            if(mode!=3) {
                int recovered=0;
                for(int y=0;y<8;++y)for(int x=-12;x<0;++x)
                    recovered+=worlds[y*W+x+(W-384)/2]==32;
                assert(recovered>0); /* Recover lit edge texels, allowing original black gaps. */
            }
        }
        /* CPU/next-frame table edits cannot rewrite already displayed inputs. */
        assert(vb_viewport_render(0,0,384,colors,levels,worlds,sources));
        memcpy(saved,levels,sizeof(saved));
        memset(s_vip_mem,0,sizeof(s_vip_mem)); s_drawing_fb=1;
        assert(vb_viewport_render(0,0,384,colors,levels,worlds,sources));
        assert(!memcmp(saved,levels,sizeof(saved)));
    }
    vb_viewport_reset();
    assert(vb_viewport_width()==384);
    assert(vb_viewport_track_extra(4,snapshot_extra));
    assert(!vb_viewport_track_extra(4,snapshot_extra));
    assert(vb_viewport_register("test.extended",32,9,0,viewport_scene,extended_layout,0));
    setup(0,0,0);
    for(int block=0;block<28;++block)vip_draw_block_into(block,left,right);
    s_display_fb=0;extra_marker=99;
    for(int eye=0;eye<2;++eye) {
        assert(vb_viewport_render(0,eye,W,colors,levels,worlds,sources));
        for(int y=0;y<8;++y)for(int x=0;x<8;++x) {
            unsigned u=7-x,v=7-y,raw=(u+2*v)%4;
            unsigned at=(y+8)*W+430+eye*2+x+(W-384)/2;
            assert(levels[at]==(raw?raw:2));
            if(raw)assert(sources[at].u==u && sources[at].v==v && sources[at].kind==3 && sources[at].raw==raw);
        }
        assert(levels[(W-384)/2-1]==3); /* Extended BG samples original x=7. */
    }
    vb_viewport_reset();
    assert(vb_viewport_register("test.adaptive",16,9,1,viewport_scene,viewport_layout,0));
    vb_viewport_window(1600,900); assert(vb_viewport_width()==398);
    vb_viewport_window(2100,900); assert(vb_viewport_width()==522);
    vb_viewport_window(3200,900); assert(vb_viewport_width()==796);
    vb_viewport_window(500,900); assert(vb_viewport_width()==384);
    vb_viewport_window(65536,1); assert(vb_viewport_width()==VB_VIEWPORT_MAX_WIDTH);
    vb_viewport_reset();
    puts("PASS: VIP viewport replay, source identity, both eyes, edge recovery, snapshot lifetime and aspect sizing");
}

int main(void) {
    for(unsigned mode=0;mode<4;++mode)for(unsigned flip=0;flip<4;++flip)for(int diagonal=0;diagonal<2;++diagonal) {
        setup(mode,flip<<12,diagonal);
        tracking=0;vip_draw_block_into(0,left,right);memcpy(baseline,left,sizeof(left));memcpy(baseline_right,right,sizeof(right));
        tracking=1;vip_draw_block_into(0,left,right);
        assert(memcmp(left,baseline,sizeof(left))==0);
        assert(memcmp(right,baseline_right,sizeof(right))==0);
        assert(vb_vip_source_buffer(0)[0].world==0); // Undisplayed drawing slot.
        s_display_fb=0;
        for(int eye=0;eye<2;++eye)for(int y=0;y<8;++y)for(int x=0;x<384;++x) {
            const VbSourceTexel* s=&vb_vip_source_buffer(eye)[y*384+x];
            const uint8_t* fb=eye?right:left;
            const unsigned pixel=(fb[x*64+y/4]>>(2*(y%4)))&3;
            if(!s->world) {assert(pixel==2);continue;}
            assert(s->kind==mode && s->world==32 && s->tile==1);
            assert(s->map==(mode==3?255:1));assert(s->u<8 && s->v<8);
            assert(s->raw==(s->u+2*s->v)%4 && s->raw==pixel);
            assert(s->tile_hash==vb_capture_hash((const uint16_t*)(s_vip_mem+0x78010)));
            if(mode!=3) {
                assert(s->x==x+(mode==1?eye+1:0));
                assert(s->y==y+((mode==2 && diagonal)?x:0));
                assert(s->u==((s->x&7)^((flip&2)?7:0)));
                assert(s->v==((s->y&7)^((flip&1)?7:0)));
            }
        }
        VbSourceTexel saved[384*8];memcpy(saved,vb_vip_source_buffer(0),sizeof(saved));
        memset(s_vip_mem+0x78010,0xff,16);s_drawing_fb=1;
        vip_draw_block_into(0,left,right);
        assert(memcmp(saved,vb_vip_source_buffer(0),sizeof(saved))==0);
    }
    setup(0,0,0);
    uint16_t* bgm=(uint16_t*)(s_vip_mem+0x20000);
    uint16_t* chr=(uint16_t*)(s_vip_mem+0x78000);
    for(int y=0;y<8;++y)chr[16+y]=0xff;
    bgm[0]=2;
    uint16_t* upper=&bgm[(0x1d800+30*32)/2];
    upper[0]=0xc000;upper[1]=4;upper[7]=7;upper[8]=7;
    bgm[(0x1d800+29*32)/2]=0x40;
    vip_draw_block_into(0,left,right);s_display_fb=0;
    for(int eye=0;eye<2;++eye)for(int y=0;y<8;++y)for(int x=4;x<12;++x) {
        const VbSourceTexel* s=&vb_vip_source_buffer(eye)[y*384+x];
        if(x<8)assert(s->world==31 && s->tile==2 && s->raw==3);
        else assert(s->world==(((x+2*y)%4)?32:0));
    }
    /* CPU software drawing shares native framebuffer ownership and lifetime.
     * A packed OR store must not steal untouched pixels from another source. */
    setup(0,0,0); s_display_fb=0;
    for(unsigned slot=0;slot<2;++slot)for(unsigned eye=0;eye<2;++eye) {
        unsigned base=slot*0x8000+eye*0x10000, at=base+64*10+4;
        vb_vip_record_cpu_write(at,0x00000003,4,101);
        assert(vb_vip_read32(at)==0); /* Observation has no guest side effect. */
        vb_vip_write32(at,0x00000003);
        vb_vip_record_cpu_write(at,0x0000000b,4,202);
        vb_vip_write32(at,0x0000000b);
        const VbSourceTexel* s=s_source_fb[slot][eye];
        assert(s[16*384+10].tile_hash==101 && s[16*384+10].raw==3);
        assert(s[17*384+10].tile_hash==202 && s[17*384+10].raw==2);
        assert(s[17*384+10].kind==4 && s[17*384+10].world==0);
        assert(s[17*384+10].x==10 && s[17*384+10].y==17);
        vb_vip_record_cpu_write(at,0x000b,2,303); /* Identical write retains owners. */
        vb_vip_write16(at,0x000b);
        assert(s[17*384+10].tile_hash==202);
        vb_vip_record_cpu_write(at,0x08,1,404); vb_vip_write8(at,0x08);
        assert(s[16*384+10].kind==0 && s[17*384+10].tile_hash==202);
        unsigned blank=base+64*10+56; /* Hidden rows never alias a visible row. */
        vb_vip_record_cpu_write(blank,0xff,1,505); vb_vip_write8(blank,0xff);
        assert(s[17*384+10].tile_hash==202);
    }
    s_drawing_fb=1;
    vip_draw_block_into(2,left,right);
    assert(s_source_fb[1][0][17*384+10].kind!=4);
    assert(s_source_fb[0][0][17*384+10].tile_hash==202);
    puts("PASS: native rasterizer and CPU source ownership, both eyes, packed stores and display lifetime");
    viewport_tests();
    return 0;
}
