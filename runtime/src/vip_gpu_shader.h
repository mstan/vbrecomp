#ifndef VB_VIP_GPU_SHADER_H
#define VB_VIP_GPU_SHADER_H
static const char* vb_vip_gpu_vertex=R"GLSL(#version 150
uniform isamplerBuffer jobs;
uniform ivec2 tile_origin;
flat out ivec4 item;
void main() {
    const ivec2 corner[6]=ivec2[6](ivec2(0,0),ivec2(1,0),ivec2(0,1),ivec2(0,1),ivec2(1,0),ivec2(1,1));
    ivec4 bounds=texelFetch(jobs,gl_InstanceID*2);
    item=texelFetch(jobs,gl_InstanceID*2+1);
    ivec2 p=ivec2(corner[gl_VertexID].x!=0?bounds.z:bounds.x,corner[gl_VertexID].y!=0?bounds.w:bounds.y);
    gl_Position=vec4(vec2(p-tile_origin)/16.0-1.0,0.0,1.0);
}
)GLSL";
static const char* vb_vip_gpu_fragment=R"GLSL(#version 150
uniform usampler2DArray source_plane;
uniform usampler2D colors;
uniform usampler2DArray bitmap;
uniform ivec2 tile_origin;
uniform int eye,scale;
flat in ivec4 item;
out vec4 result;
uint word(uint at){at&=65535u;return texelFetch(source_plane,ivec3(int(at&255u),int(at>>8u),item.w),0).r;}
uint chr(uint at){return texelFetch(source_plane,ivec3(int(at&255u),256+int(at>>8u),item.w),0).r;}
uint meta(int at){return texelFetch(source_plane,ivec3(at,320,item.w),0).r;}
int sign_bits(uint n,uint bits){uint b=1u<<(bits-1u);return int((n&((1u<<bits)-1u))^b)-int(b);}
int half(uint at){return sign_bits(word(at),16u);}
int floor_div(int n,int d){return n>=0?n/d:-((-n+d-1)/d);}
void main() {
    ivec2 local=ivec2(gl_FragCoord.xy),p=tile_origin+local/scale;
    int kind=item.x;uint level=0u;
    if(kind==-2) {
        uint b=texelFetch(bitmap,ivec3(p.y/4,p.x,eye),0).r;
        level=(b>>uint((p.y&3)*2))&3u;
    } else if(kind==-1)level=meta(0)&3u;
    else {
        uint cell=0u,xx=0u,yy=0u;
        if(kind==3) {
            uint at=61440u+uint(item.z)*4u;
            uint jp=word(at+1u)&16383u;
            int gx=sign_bits(word(at)+(eye!=0?jp:0u-jp),10u);
            cell=word(at+3u);
            xx=uint(p.x-gx)^((cell&8192u)!=0u?7u:0u);
            yy=uint((p.y-int(word(at+2u)))&255)^((cell&4096u)!=0u?7u:0u);
        } else {
            uint at=60416u+uint(item.y)*16u,w0=word(at);
            int gp=sign_bits(word(at+2u),9u);
            int gx=sign_bits(uint(sign_bits(word(at+1u),11u)+(eye!=0?gp:-gp)),10u);
            int gy=sign_bits(word(at+3u),11u);
            uint ry=uint(p.y-gy)&65535u,scx=(w0>>10u)&3u,scy=(w0>>8u)&3u;
            uint xs=512u<<scx,ys=512u<<scy,base=(w0&15u)<<12u;
            bool over=(w0&128u)!=0u;cell=word(word(at+10u));
            if(kind==2) {
                uint param=(word(at+9u)&65520u)+8u*ry;
                int mx=half(param),mp=half(param+1u),my=half(param+2u),dx=half(param+3u),dy=half(param+4u);
                int shift=mp>=0&&eye!=0?mp:mp<0&&eye==0?-mp:0;
                // Split native integer and subpixel phases: bounded products at64x.
                int phase=local.x%scale;
                uint u=uint(mx*64+dx*(p.x-gx+shift))+uint(floor_div(dx*phase,scale));
                uint v=uint(my*64+dy*(p.x-gx+shift))+uint(floor_div(dy*phase,scale));
                xx=u&(over?67108863u:(xs<<9u)-1u);yy=v&(over?67108863u:(ys<<9u)-1u);
                if(dy==0 && yy>=(ys<<9u))discard;
                if(xx<(xs<<9u) && yy<(ys<<9u))cell=word(base|((xx>>6u)&~4095u)|(((yy>>6u)&~4095u)<<scx)|((xx>>12u)&63u)|(((yy>>12u)&63u)*64u));
                xx>>=9u;yy>>=9u;
            } else {
                int mp=sign_bits(word(at+5u),9u);
                int sx=sign_bits(word(at+4u)+uint(eye!=0?mp:-mp),16u);
                if(kind==1)sx=sign_bits(uint(sx+half((word(at+9u)&65520u)+((ry*2u)|uint(eye)))),16u);
                xx=uint(sx+p.x-gx)&(over?8191u:xs-1u);
                yy=((word(at+6u)+ry)&65535u)&(over?8191u:ys-1u);
                if(xx<xs && yy<ys)cell=word(base|((xx<<3u)&~4095u)|(((yy<<3u)&~4095u)<<scx)|((xx>>3u)&63u)|(((yy>>3u)&63u)*64u));
            }
            xx=(xx&7u)^((cell&8192u)!=0u?7u:0u);yy=(yy&7u)^((cell&4096u)!=0u?7u:0u);
        }
        uint raw=(chr((cell&2047u)*8u+yy)>>(xx*2u))&3u;
        if(raw==0u)discard;
        level=meta((kind==3?17:1)+int(cell>>14u)*4+int(raw));
    }
    uint argb=texelFetch(colors,ivec2(int(level&3u),eye*96+p.x/4),0).r;
    result=vec4(vec3((argb>>16u)&255u,(argb>>8u)&255u,argb&255u)/255.0,1.0);
}
)GLSL";
#endif
