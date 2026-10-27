/* Byte-for-byte differential across independently compiled raster paths.
 * Snapshots vary between eight-row blocks; all four output planes must agree.
 */
#include "viewport.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ref_viewport_track(void);
int ref_viewport_register(const char*,unsigned,unsigned,int,VbViewportClassify,VbViewportArrange,void*);
void ref_viewport_capture(int,int,const uint16_t*,const uint16_t*,const uint16_t*,
    const uint8_t (*)[4],const uint8_t (*)[4],unsigned,const uint16_t*);
int ref_viewport_render(int,int,int,uint32_t*,uint8_t*,uint16_t*,VbSourceTexel*);
int vb_vip_display_fb(void) { return 0; }
void vb_vip_column_argb(int eye,uint32_t colors[96][4]) {
    for(unsigned g=0;g<96;++g)for(unsigned i=0;i<4;++i)
        colors[g][i]=0xff000000u|((g*17+i*53+eye*7)&255)<<16;
}
static uint32_t seed=0x19bcd71u;
static unsigned random_value(void) { seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;return seed; }
static int classify(const VbViewportScene* s,void* c) { (void)s;(void)c;return 1; }
static int outside(int x,int y,int* sx,int* sy,void* c) {
    (void)c;*sx=x*3+7;*sy=y+(x&3);return (x&15)!=2;
}
static int cell(int x,int y,uint16_t* out,void* c) {
    (void)c;*out=(uint16_t)(x*91+y*17);return (x&7)!=1;
}
static unsigned scene;
static void arrange(const VbViewportScene* s,int kind,int width,VbViewportLayout* l,void* c) {
    (void)s;(void)kind;(void)width;(void)c;
    l->camera_shift_x=(int)(scene%13)-6;
    for(int w=0;w<32;++w) {
        l->worlds[w].top=scene%11;l->worlds[w].bottom=219;
        l->worlds[w].left_end=101;l->worlds[w].right_start=307;
        l->worlds[w].left_shift=-23;l->worlds[w].center_shift=5;l->worlds[w].right_shift=31;
        if(scene%3==0)l->worlds[w].outside=outside;
        if(scene%3==1)l->worlds[w].outside_cell=cell;
    }
}
int main(void) {
    enum {MAX=796*224};
    uint32_t* a=malloc(MAX*4),*b=malloc(MAX*4);
    uint8_t* al=malloc(MAX),*bl=malloc(MAX);
    uint16_t* aw=malloc(MAX*2),*bw=malloc(MAX*2);
    VbSourceTexel* as=malloc(MAX*sizeof(*as)),*bs=malloc(MAX*sizeof(*bs));
    uint16_t* dram=malloc(131072),*chars=malloc(32768),spt[4]={127,255,511,1023},suppress[1024]={0};
    assert(a&&b&&al&&bl&&aw&&bw&&as&&bs&&dram&&chars);
    vb_viewport_track();ref_viewport_track();
    assert(vb_viewport_register("jobs",32,9,0,classify,arrange,0));
    assert(ref_viewport_register("scalar",32,9,0,classify,arrange,0));
    for(scene=0;scene<48;++scene) {
        for(int i=0;i<65536;++i)dram[i]=(uint16_t)random_value();
        for(int i=0;i<16384;++i)chars[i]=(uint16_t)random_value();
        uint8_t gplt[4][4],jplt[4][4];
        for(int p=0;p<4;++p)for(int i=0;i<4;++i) {gplt[p][i]=random_value()&3;jplt[p][i]=random_value()&3;}
        uint16_t* worlds=dram+0x1d800/2;
        for(int w=31;w>=28;--w) {
            uint16_t* v=worlds+w*16;
            v[0]=(uint16_t)(0xc000u|((scene+w)%3)<<12|((scene>>2)&3)<<10|((scene>>4)&3)<<8|(scene&1?128:0)|(w&15));
            v[1]=(uint16_t)((int)(scene*11%89)-37);v[2]=scene*7%31;v[3]=0;
            v[4]=(uint16_t)((int)(scene*37)-1900);v[5]=(uint16_t)(scene*5-100);v[6]=scene*13;
            v[7]=383;v[8]=223;v[9]=0x1000;v[10]=scene*947;
        }
        worlds[27*16]=64;
        for(int row=0;row<28;++row) {
            chars[(row*53)%16384]^=(uint16_t)(scene+row+1);
            vb_viewport_capture(0,row,dram,chars,spt,(const uint8_t (*)[4])gplt,
                (const uint8_t (*)[4])jplt,scene&3,suppress);
            ref_viewport_capture(0,row,dram,chars,spt,(const uint8_t (*)[4])gplt,
                (const uint8_t (*)[4])jplt,scene&3,suppress);
        }
        const int widths[]={384,398,522,796};
        for(unsigned k=0;k<4;++k)for(int eye=0;eye<2;++eye) {
            int width=widths[k];size_t n=(size_t)width*224;
            assert(vb_viewport_render(0,eye,width,a,al,aw,as));
            assert(ref_viewport_render(0,eye,width,b,bl,bw,bs));
            assert(!memcmp(a,b,n*4));assert(!memcmp(al,bl,n));
            assert(!memcmp(aw,bw,n*2));assert(!memcmp(as,bs,n*sizeof(*as)));
        }
    }
    free(a);free(b);free(al);free(bl);free(aw);free(bw);free(as);free(bs);free(dram);free(chars);
    puts("PASS: 384 scalar/job comparisons, both eyes and four output planes; block mutations, wraps, flips, HBias, affine and arbitrary outside callbacks");
    return 0;
}
