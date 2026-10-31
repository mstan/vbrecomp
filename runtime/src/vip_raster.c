#include "vip_raster.h"
#include <string.h>
static int sign_bits(unsigned x,unsigned n){unsigned b=1u<<(n-1);return (int)((x&((1u<<n)-1))^b)-(int)b;}
static int max_int(int a,int b){return a>b?a:b;}
static int min_int(int a,int b){return a<b?a:b;}
static int emit(VbVipRasterJob* out,unsigned cap,unsigned* n,int kind,int world,int object,int bank,int x0,int y0,int x1,int y1) {
    x0=max_int(x0,0);x1=min_int(x1,384);y0=max_int(y0,bank*8);y1=min_int(y1,bank*8+8);
    if(x0>=x1 || y0>=y1)return 1;
    if(*n>=cap)return 0;
    out[(*n)++]=(VbVipRasterJob){{x0,y0,x1,y1},{kind,world,object,bank}};return 1;
}
int vb_vip_raster_prepare(const VbVipRasterBank banks[28],int eye,uint32_t bitmap_rows,
                         VbVipRasterJob* jobs,unsigned cap,unsigned* count) {
    if(!banks || !jobs || !count || cap>VB_VIP_RASTER_JOB_LIMIT || (unsigned)eye>1)return 0;
    unsigned n=0;
    for(int row=0;row<28;++row) {
        if(bitmap_rows&(1u<<row)) {
            if(!emit(jobs,cap,&n,-2,0,0,row,0,row*8,384,row*8+8))return 0;
            continue;
        }
        const VbVipDrawInputs* b=&banks[row].input;
        if(!b->dram || !b->chars || !b->spt || !b->suppress || !b->gplt || !b->jplt)return 0;
        if(!emit(jobs,cap,&n,-1,0,0,row,0,row*8,384,row*8+8))return 0;
        int group=3;
        for(int world=31;world>=0;--world) {
            const uint16_t* w=b->dram+0x1d800/2+world*16;
            if(w[0]&64)break;
            int kind=(w[0]>>12)&3;
            if(w[0]&(eye?0x4000:0x8000)) {
                if(kind==3) {
                    int at=b->spt[group],end=group?b->spt[group-1]:1023;
                    do {
                        const uint16_t* o=b->dram+(0x1e000+at*8)/2;
                        if(!(o[1]&(eye?0x4000:0x8000)) || b->suppress[at]==(o[3]&2047)+1)continue;
                        int x=sign_bits(o[0]+(eye?(o[1]&0x3fff):-(o[1]&0x3fff)),10);
                        if(x<=-8 || x>=384)continue;
                        int lo=224,hi=0;
                        for(int y=row*8;y<row*8+8;++y)if(((y-o[2])&255)<8){lo=min_int(lo,y);hi=y+1;}
                        if(!emit(jobs,cap,&n,3,world,at,row,x,lo,x+8,hi))return 0;
                    }while((at=(at-1)&1023)!=end);
                } else {
                    int gp=sign_bits(w[2],9);
                    int gx=sign_bits(sign_bits(w[1],11)+(eye?gp:-gp),10),gy=sign_bits(w[3],11);
                    unsigned width=(uint16_t)sign_bits(w[7],11);
                    if(!emit(jobs,cap,&n,kind,world,0,row,gx,gy,gx+(int)width+1,gy+(w[8]&1023)+1))return 0;
                }
            }
            if(kind==3 && group)--group;
        }
    }
    *count=n;return 1;
}
static int sample(const VbVipRasterBank* bank,const VbVipRasterJob* j,int eye,int x,int y,unsigned* level) {
    const VbVipDrawInputs* b=&bank->input;int kind=j->data[0];
    if(kind==-1){*level=b->backdrop&3;return 1;}
    const uint16_t* d=b->dram;
    unsigned cell,xx,yy;
    if(kind==3) {
        const uint16_t* o=d+(0x1e000+j->data[2]*8)/2;
        int gx=sign_bits(o[0]+(eye?(o[1]&0x3fff):-(o[1]&0x3fff)),10);
        xx=(unsigned)(x-gx)^((o[3]&0x2000)?7u:0u);
        yy=((y-o[2])&255)^((o[3]&0x1000)?7u:0u);cell=o[3];
    } else {
        const uint16_t* w=d+0x1d800/2+j->data[1]*16;
        int gp=sign_bits(w[2],9),gx=sign_bits(sign_bits(w[1],11)+(eye?gp:-gp),10),gy=sign_bits(w[3],11);
        unsigned ry=(uint16_t)(y-gy),scx=(w[0]>>10)&3,scy=(w[0]>>8)&3;
        unsigned xs=512u<<scx,ys=512u<<scy,base=(w[0]&15)<<12;int over=(w[0]&128)!=0;
        cell=d[w[10]];
        if(kind==2) {
            unsigned p=((w[9]&0xfff0)+8*ry)&65535;
            int mx=(int16_t)d[p],mp=(int16_t)d[(p+1)&65535],my=(int16_t)d[(p+2)&65535];
            int dx=(int16_t)d[(p+3)&65535],dy=(int16_t)d[(p+4)&65535];
            int shift=mp>=0&&eye?mp:mp<0&&!eye?-mp:0;
            xx=(uint32_t)(mx*64+dx*(x-gx+shift))&(over?0x3ffffffu:(xs<<9)-1);
            yy=(uint32_t)(my*64+dy*(x-gx+shift))&(over?0x3ffffffu:(ys<<9)-1);
            if(!dy && yy>=(ys<<9))return 0;
            if(xx<(xs<<9) && yy<(ys<<9))cell=d[(base|((xx>>6)&~4095u)|(((yy>>6)&~4095u)<<scx)|((xx>>12)&63)|(((yy>>12)&63)*64))&65535];
            xx>>=9;yy>>=9;
        } else {
            int mp=sign_bits(w[5],9);int sx=(int16_t)(w[4]+(eye?mp:-mp));
            if(kind==1)sx=(int16_t)(sx+(int16_t)d[((w[9]&0xfff0)+((ry*2)|eye))&65535]);
            xx=(unsigned)(sx+x-gx)&(over?8191u:xs-1);yy=(uint16_t)(w[6]+ry)&(over?8191u:ys-1);
            if(xx<xs && yy<ys)cell=d[(base|((xx<<3)&~4095u)|(((yy<<3)&~4095u)<<scx)|((xx>>3)&63)|(((yy>>3)&63)*64))&65535];
        }
        xx=(xx&7)^((cell&0x2000)?7u:0u);yy=(yy&7)^((cell&0x1000)?7u:0u);
    }
    unsigned raw=(b->chars[(cell&2047)*8+yy]>>(xx*2))&3;
    if(!raw)return 0;
    *level=kind==3?b->jplt[cell>>14][raw]:b->gplt[cell>>14][raw];return 1;
}
int vb_vip_raster_cpu(const VbVipRasterBank banks[28],const VbVipRasterJob* jobs,unsigned count,
                      int eye,const uint8_t* bitmap,uint8_t* levels) {
    if(!banks || !jobs || !levels || count>VB_VIP_RASTER_JOB_LIMIT || (unsigned)eye>1)return 0;
    memset(levels,0,384*224);
    for(unsigned i=0;i<count;++i) {
        const VbVipRasterJob* j=jobs+i;
        for(int y=j->bounds[1];y<j->bounds[3];++y)for(int x=j->bounds[0];x<j->bounds[2];++x) {
            unsigned value;
            if(j->data[0]==-2) {if(!bitmap)return 0;value=(bitmap[x*64+y/4]>>((y&3)*2))&3;}
            else if(!sample(banks+j->data[3],j,eye,x,y,&value))continue;
            levels[y*384+x]=(uint8_t)value;
        }
    }
    return 1;
}
