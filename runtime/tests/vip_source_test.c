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
    puts("PASS: BG/HBias/affine/OBJ, flips, both eyes, native pixels and displayed source lifetime");
    return 0;
}
