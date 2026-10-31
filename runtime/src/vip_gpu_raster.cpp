#include "vip_gpu_raster.h"
#include "vip.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>
#ifndef VB_RUNTIME_HAVE_SDL
#define VB_RUNTIME_HAVE_SDL 0
#endif
#if VB_RUNTIME_HAVE_SDL
#include <SDL.h>
#if SDL_VERSION_ATLEAST(2,0,10)
#include <SDL_opengl.h>
#define VB_VIP_GL 1
#endif
#endif
#ifdef VB_VIP_GL
#include "vip_gpu_shader.h"
#define VB_GL_FUNCTIONS(X) \
 X(GLenum,GetError,()) X(void,GetIntegerv,(GLenum,GLint*)) \
 X(void,Enable,(GLenum)) X(void,Disable,(GLenum)) X(void,Viewport,(GLint,GLint,GLsizei,GLsizei)) \
 X(void,ClearColor,(GLfloat,GLfloat,GLfloat,GLfloat)) X(void,Clear,(GLbitfield)) \
 X(void,ActiveTexture,(GLenum)) X(void,GenTextures,(GLsizei,GLuint*)) X(void,DeleteTextures,(GLsizei,const GLuint*)) \
 X(void,BindTexture,(GLenum,GLuint)) X(void,TexParameteri,(GLenum,GLenum,GLint)) \
 X(void,PixelStorei,(GLenum,GLint)) \
 X(void,TexImage2D,(GLenum,GLint,GLint,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*)) \
 X(void,TexSubImage2D,(GLenum,GLint,GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,const void*)) \
 X(void,TexImage3D,(GLenum,GLint,GLint,GLsizei,GLsizei,GLsizei,GLint,GLenum,GLenum,const void*)) \
 X(void,TexSubImage3D,(GLenum,GLint,GLint,GLint,GLint,GLsizei,GLsizei,GLsizei,GLenum,GLenum,const void*)) \
 X(void,GenBuffers,(GLsizei,GLuint*)) X(void,DeleteBuffers,(GLsizei,const GLuint*)) \
 X(void,BindBuffer,(GLenum,GLuint)) X(void,BufferData,(GLenum,GLsizeiptr,const void*,GLenum)) \
 X(void,TexBuffer,(GLenum,GLenum,GLuint)) \
 X(GLuint,CreateShader,(GLenum)) X(void,ShaderSource,(GLuint,GLsizei,const GLchar*const*,const GLint*)) \
 X(void,CompileShader,(GLuint)) X(void,GetShaderiv,(GLuint,GLenum,GLint*)) \
 X(void,GetShaderInfoLog,(GLuint,GLsizei,GLsizei*,GLchar*)) X(void,DeleteShader,(GLuint)) \
 X(GLuint,CreateProgram,()) X(void,AttachShader,(GLuint,GLuint)) X(void,LinkProgram,(GLuint)) \
 X(void,BindFragDataLocation,(GLuint,GLuint,const GLchar*)) X(void,GetProgramiv,(GLuint,GLenum,GLint*)) \
 X(void,UseProgram,(GLuint)) X(void,DeleteProgram,(GLuint)) X(GLint,GetUniformLocation,(GLuint,const GLchar*)) \
 X(void,Uniform1i,(GLint,GLint)) X(void,Uniform2i,(GLint,GLint,GLint)) \
 X(void,GenVertexArrays,(GLsizei,GLuint*)) X(void,DeleteVertexArrays,(GLsizei,const GLuint*)) X(void,BindVertexArray,(GLuint)) \
 X(void,DrawArraysInstanced,(GLenum,GLint,GLsizei,GLsizei)) \
 X(void,GenFramebuffers,(GLsizei,GLuint*)) X(void,DeleteFramebuffers,(GLsizei,const GLuint*)) \
 X(void,BindFramebuffer,(GLenum,GLuint)) X(void,FramebufferTexture2D,(GLenum,GLenum,GLenum,GLuint,GLint)) \
 X(GLenum,CheckFramebufferStatus,(GLenum)) \
 X(void,BlitFramebuffer,(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum)) \
 X(void,ReadPixels,(GLint,GLint,GLsizei,GLsizei,GLenum,GLenum,void*))
namespace {
struct Gl {
#define FIELD(ret,name,args) using name##Fn=ret(APIENTRY*)args;name##Fn name{};
    VB_GL_FUNCTIONS(FIELD)
#undef FIELD
    bool load(){
#define LOAD(ret,name,args) name=reinterpret_cast<name##Fn>(SDL_GL_GetProcAddress("gl" #name));if(!name)return false;
        VB_GL_FUNCTIONS(LOAD)
#undef LOAD
        return true;
    }
};
struct Context {
    SDL_Window* old_window=SDL_GL_GetCurrentWindow();SDL_GLContext old=SDL_GL_GetCurrentContext();bool good;
    Context(SDL_Window* w,SDL_GLContext c):good(c && (old==c || SDL_GL_MakeCurrent(w,c)==0)){}
    ~Context(){if(SDL_GL_GetCurrentContext()!=old)SDL_GL_MakeCurrent(old?old_window:nullptr,old);}
};
}
struct VbVipGpuRaster::Impl {
    Gl gl;SDL_Window* window{};SDL_GLContext context{};
    GLuint program{},vao{},images[4]{},job_buffer{},fbo{};unsigned tile_size{};int swap_interval=-2;
    GLint tile_uniform{},eye_uniform{},scale_uniform{},max_scale{};
    uint64_t pages[28][40]{},bitmap_versions[2]{};
    std::array<std::array<uint16_t,33>,28> metadata{};
    std::array<uint32_t,2*96*4> colors{};
    bool sources_initialized=false,bank_initialized[28]{},colors_initialized=false,failed=false;
    VbVipGpuRasterCosts costs{};
    using GenQueriesFn=void(APIENTRY*)(GLsizei,GLuint*);using DeleteQueriesFn=void(APIENTRY*)(GLsizei,const GLuint*);
    using QueryCounterFn=void(APIENTRY*)(GLuint,GLenum);using GetQueryObjectivFn=void(APIENTRY*)(GLuint,GLenum,GLint*);
    using GetQueryObjectui64vFn=void(APIENTRY*)(GLuint,GLenum,GLuint64*);using GetQueryivFn=void(APIENTRY*)(GLenum,GLenum,GLint*);
    GenQueriesFn gen_queries{};DeleteQueriesFn delete_queries{};QueryCounterFn query_counter{};
    GetQueryObjectivFn query_available{};GetQueryObjectui64vFn query_value{};GetQueryivFn query_bits{};
    GLuint queries[8]{};bool pending[4]{},query_good[4]{};
    void collect_times(){
        if(!costs.timer_available)return;
        for(unsigned i=0;i<4;++i)if(pending[i]) {
            GLint ready=0;query_available(queries[i*2+1],GL_QUERY_RESULT_AVAILABLE,&ready);if(!ready)continue;
            GLuint64 a=0,b=0;query_value(queries[i*2],GL_QUERY_RESULT,&a);query_value(queries[i*2+1],GL_QUERY_RESULT,&b);
            if(query_good[i] && b>=a){uint64_t n=b-a;costs.gpu_nanoseconds+=n;costs.gpu_max_nanoseconds=std::max(costs.gpu_max_nanoseconds,n);++costs.timed_frames;}
            pending[i]=false;--costs.timer_pending;
        }
    }
    int begin_time(){
        collect_times();if(!costs.timer_available)return -1;
        for(int i=0;i<4;++i)if(!pending[i]){query_counter(queries[i*2],GL_TIMESTAMP);return i;}
        ++costs.timer_skipped;return -1;
    }
    void end_time(int i,bool good){if(i<0)return;query_counter(queries[i*2+1],GL_TIMESTAMP);pending[i]=true;query_good[i]=good;++costs.timer_pending;}
    void uploaded(uint64_t bytes){costs.upload_bytes+=bytes;costs.last_upload_bytes+=bytes;}
    std::vector<VbVipRasterJob> jobs[2];
    ~Impl(){
        SDL_Window* previous_window=SDL_GL_GetCurrentWindow();SDL_GLContext previous=SDL_GL_GetCurrentContext();
        const bool active=context && (previous==context || SDL_GL_MakeCurrent(window,context)==0);
        if(active && delete_queries && queries[0])delete_queries(8,queries);
        if(active && gl.DeleteTextures){gl.DeleteTextures(4,images);if(tile_image)gl.DeleteTextures(1,&tile_image);gl.DeleteBuffers(1,&job_buffer);gl.DeleteFramebuffers(1,&fbo);gl.DeleteVertexArrays(1,&vao);if(program)gl.DeleteProgram(program);}
        if(context){SDL_GLContext dying=context;context=nullptr;if(previous==dying)previous=nullptr;SDL_GL_MakeCurrent(nullptr,nullptr);SDL_GL_DeleteContext(dying);}
        SDL_GL_MakeCurrent(previous?previous_window:nullptr,previous);
    }
    GLuint shader(GLenum kind,const char* body) {
        GLuint s=gl.CreateShader(kind);gl.ShaderSource(s,1,&body,nullptr);gl.CompileShader(s);GLint good=0;gl.GetShaderiv(s,GL_COMPILE_STATUS,&good);
        if(!good){char log[2048]{};gl.GetShaderInfoLog(s,sizeof(log),nullptr,log);std::fprintf(stderr,"VIP GPU shader unavailable: %s\n",log);gl.DeleteShader(s);return 0;}return s;
    }
    void bind(unsigned unit,GLenum target,GLuint value){gl.ActiveTexture(GL_TEXTURE0+unit);gl.BindTexture(target,value);}
    bool start() {
        if(!gl.load())return false;
        GLint version=0;gl.GetIntegerv(GL_MAJOR_VERSION,&version);if(version<3)return false;
        GLint limit=0,layers=0;gl.GetIntegerv(GL_MAX_TEXTURE_SIZE,&limit);gl.GetIntegerv(GL_MAX_ARRAY_TEXTURE_LAYERS,&layers);
        if(limit<321 || layers<28)return false;
        max_scale=std::min(64,limit/32);
        if(SDL_GL_ExtensionSupported("GL_ARB_timer_query")){
            gen_queries=reinterpret_cast<GenQueriesFn>(SDL_GL_GetProcAddress("glGenQueries"));
            delete_queries=reinterpret_cast<DeleteQueriesFn>(SDL_GL_GetProcAddress("glDeleteQueries"));
            query_counter=reinterpret_cast<QueryCounterFn>(SDL_GL_GetProcAddress("glQueryCounter"));
            query_available=reinterpret_cast<GetQueryObjectivFn>(SDL_GL_GetProcAddress("glGetQueryObjectiv"));
            query_value=reinterpret_cast<GetQueryObjectui64vFn>(SDL_GL_GetProcAddress("glGetQueryObjectui64v"));
            query_bits=reinterpret_cast<GetQueryivFn>(SDL_GL_GetProcAddress("glGetQueryiv"));
            if(gen_queries && delete_queries && query_counter && query_available && query_value && query_bits){
                GLint bits=0;query_bits(GL_TIMESTAMP,GL_QUERY_COUNTER_BITS,&bits);if(bits>0){gen_queries(8,queries);costs.timer_available=true;}
            }
        }
        GLuint v=shader(GL_VERTEX_SHADER,vb_vip_gpu_vertex),f=shader(GL_FRAGMENT_SHADER,vb_vip_gpu_fragment);
        if(!v || !f){if(v)gl.DeleteShader(v);if(f)gl.DeleteShader(f);return false;}
        program=gl.CreateProgram();gl.AttachShader(program,v);gl.AttachShader(program,f);gl.BindFragDataLocation(program,0,"result");gl.LinkProgram(program);
        gl.DeleteShader(v);gl.DeleteShader(f);GLint good=0;gl.GetProgramiv(program,GL_LINK_STATUS,&good);if(!good)return false;
        gl.GenVertexArrays(1,&vao);gl.BindVertexArray(vao);gl.GenTextures(4,images);gl.GenBuffers(1,&job_buffer);gl.GenFramebuffers(1,&fbo);
        bind(0,GL_TEXTURE_2D_ARRAY,images[0]);gl.TexImage3D(GL_TEXTURE_2D_ARRAY,0,GL_R16UI,256,321,28,0,GL_RED_INTEGER,GL_UNSIGNED_SHORT,nullptr);
        gl.TexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_NEAREST);gl.TexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        bind(1,GL_TEXTURE_BUFFER,images[1]);gl.BindBuffer(GL_TEXTURE_BUFFER,job_buffer);gl.BufferData(GL_TEXTURE_BUFFER,sizeof(VbVipRasterJob),nullptr,GL_STREAM_DRAW);gl.TexBuffer(GL_TEXTURE_BUFFER,GL_RGBA32I,job_buffer);
        bind(2,GL_TEXTURE_2D,images[2]);gl.TexImage2D(GL_TEXTURE_2D,0,GL_R32UI,4,192,0,GL_RED_INTEGER,GL_UNSIGNED_INT,nullptr);
        gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        bind(3,GL_TEXTURE_2D_ARRAY,images[3]);gl.TexImage3D(GL_TEXTURE_2D_ARRAY,0,GL_R8UI,64,384,2,0,GL_RED_INTEGER,GL_UNSIGNED_BYTE,nullptr);
        gl.TexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_NEAREST);gl.TexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        gl.UseProgram(program);gl.Uniform1i(gl.GetUniformLocation(program,"source_plane"),0);gl.Uniform1i(gl.GetUniformLocation(program,"jobs"),1);
        gl.Uniform1i(gl.GetUniformLocation(program,"colors"),2);gl.Uniform1i(gl.GetUniformLocation(program,"bitmap"),3);
        tile_uniform=gl.GetUniformLocation(program,"tile_origin");eye_uniform=gl.GetUniformLocation(program,"eye");scale_uniform=gl.GetUniformLocation(program,"scale");
        gl.PixelStorei(GL_UNPACK_ALIGNMENT,1);gl.PixelStorei(GL_PACK_ALIGNMENT,1);
        gl.Disable(GL_BLEND);gl.Disable(GL_DEPTH_TEST);gl.Disable(GL_STENCIL_TEST);gl.Disable(GL_CULL_FACE);gl.Disable(GL_DITHER);gl.Disable(GL_FRAMEBUFFER_SRGB);
        return gl.GetError()==GL_NO_ERROR;
    }
    GLuint tile_image{};
    bool tile(unsigned scale) {
        if(!scale || scale>(unsigned)max_scale)return false;
        unsigned size=32*scale;
        if(size!=tile_size){if(!tile_image)gl.GenTextures(1,&tile_image);bind(4,GL_TEXTURE_2D,tile_image);gl.TexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,size,size,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
            gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);gl.TexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);tile_size=size;}
        gl.BindFramebuffer(GL_FRAMEBUFFER,fbo);gl.FramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,tile_image,0);
        return gl.CheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE && gl.GetError()==GL_NO_ERROR;
    }
    bool upload(const VbVipGpuFrame& frame) {
        bind(0,GL_TEXTURE_2D_ARRAY,images[0]);
        for(unsigned row=0;row<28;++row) {
            const auto& bank=frame.banks[row];
            if(!bank.input.dram || !bank.input.chars)continue;
            for(unsigned page=0;page<40;++page) {
                uint64_t revision=bank.pages?bank.pages[page]:0;
                if(sources_initialized && revision && pages[row][page]==revision)continue;
                const uint16_t* source=page<32?bank.input.dram+page*2048:bank.input.chars+(page-32)*2048;
                gl.TexSubImage3D(GL_TEXTURE_2D_ARRAY,0,0,page*8,row,256,8,1,GL_RED_INTEGER,GL_UNSIGNED_SHORT,source);uploaded(4096);pages[row][page]=revision;
            }
            std::array<uint16_t,33> next{};next[0]=bank.input.backdrop&3;
            for(unsigned p=0;p<4;++p)for(unsigned i=0;i<4;++i){next[1+p*4+i]=bank.input.gplt[p][i];next[17+p*4+i]=bank.input.jplt[p][i];}
            if(!bank_initialized[row] || metadata[row]!=next){gl.TexSubImage3D(GL_TEXTURE_2D_ARRAY,0,0,320,row,33,1,1,GL_RED_INTEGER,GL_UNSIGNED_SHORT,next.data());uploaded(sizeof(next));metadata[row]=next;}
            bank_initialized[row]=true;
        }
        sources_initialized=true;
        if(!colors_initialized || std::memcmp(colors.data(),frame.colors,sizeof(frame.colors))){bind(2,GL_TEXTURE_2D,images[2]);gl.TexSubImage2D(GL_TEXTURE_2D,0,0,0,4,192,GL_RED_INTEGER,GL_UNSIGNED_INT,frame.colors);uploaded(sizeof(frame.colors));std::memcpy(colors.data(),frame.colors,sizeof(frame.colors));colors_initialized=true;}
        bind(3,GL_TEXTURE_2D_ARRAY,images[3]);
        for(unsigned eye=0;eye<2;++eye)if(frame.bitmaps[eye] && (!frame.bitmap_revision[eye] || bitmap_versions[eye]!=frame.bitmap_revision[eye])) {
            gl.TexSubImage3D(GL_TEXTURE_2D_ARRAY,0,0,0,eye,64,384,1,GL_RED_INTEGER,GL_UNSIGNED_BYTE,frame.bitmaps[eye]);uploaded(64*384);bitmap_versions[eye]=frame.bitmap_revision[eye];
        }
        return gl.GetError()==GL_NO_ERROR;
    }
    bool eye_jobs(const VbVipGpuFrame& frame,int eye) {
        if(!frame.jobs[eye] || frame.counts[eye]>VB_VIP_RASTER_JOB_LIMIT)return false;
        GLint maximum=0;gl.GetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE,&maximum);if(frame.counts[eye]*2u>(unsigned)maximum)return false;
        bind(1,GL_TEXTURE_BUFFER,images[1]);gl.BindBuffer(GL_TEXTURE_BUFFER,job_buffer);
        gl.BufferData(GL_TEXTURE_BUFFER,frame.counts[eye]*sizeof(VbVipRasterJob),frame.jobs[eye],GL_STREAM_DRAW);uploaded(frame.counts[eye]*sizeof(VbVipRasterJob));gl.TexBuffer(GL_TEXTURE_BUFFER,GL_RGBA32I,job_buffer);
        gl.UseProgram(program);gl.Uniform1i(eye_uniform,eye);gl.BindVertexArray(vao);return gl.GetError()==GL_NO_ERROR;
    }
    void draw_tile(unsigned tx,unsigned ty,unsigned scale,unsigned count) {
        gl.BindFramebuffer(GL_FRAMEBUFFER,fbo);gl.Viewport(0,0,tile_size,tile_size);gl.ClearColor(0,0,0,1);gl.Clear(GL_COLOR_BUFFER_BIT);
        gl.UseProgram(program);gl.Uniform2i(tile_uniform,tx*32,ty*32);gl.Uniform1i(scale_uniform,scale);
        gl.DrawArraysInstanced(GL_TRIANGLES,0,6,count);
    }
};
VbVipGpuRaster::VbVipGpuRaster()=default;VbVipGpuRaster::~VbVipGpuRaster()=default;
bool VbVipGpuRaster::initialize(SDL_Window* window) {
    if(attempted_ && attempted_window_==window)return impl_ && !impl_->failed;
    impl_.reset();attempted_=true;attempted_window_=window;
    if(!window || !(SDL_GetWindowFlags(window)&SDL_WINDOW_OPENGL)){++init_failures_;return false;}
    SDL_Window* old_window=SDL_GL_GetCurrentWindow();SDL_GLContext old=SDL_GL_GetCurrentContext();
    int major=0,minor=0,profile=0,share=0;
    SDL_GL_GetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,&major);SDL_GL_GetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,&minor);SDL_GL_GetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,&profile);SDL_GL_GetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT,&share);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,3);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,2);SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,SDL_GL_CONTEXT_PROFILE_CORE);SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT,0);
    auto candidate=std::make_unique<Impl>();candidate->window=window;candidate->context=SDL_GL_CreateContext(window);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION,major);SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION,minor);SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK,profile);SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT,share);
    bool good=candidate->context && candidate->start();
    SDL_GL_MakeCurrent(old?old_window:nullptr,old);
    if(!good){++init_failures_;std::fputs("VIP GPU unavailable; using portable presentation until reset/window replacement.\n",stderr);return false;}
    impl_=std::move(candidate);return true;
}
bool VbVipGpuRaster::present(const VbVipGpuFrame& frame,bool stereo,unsigned requested,int width,int height,bool vsync) {
    if(!impl_ || impl_->failed || width<=0 || height<=0)return false;
    auto& s=*impl_;Context use(s.window,s.context);if(!use.good)return false;
    if(s.swap_interval!=(vsync?1:0)){SDL_GL_SetSwapInterval(vsync?1:0);s.swap_interval=vsync?1:0;}
    unsigned scale=std::min(requested,(unsigned)s.max_scale);
    if(s.costs.requested_scale!=requested || s.costs.effective_scale!=scale){
        std::fprintf(stderr,"VIP GPU requested scale=%u effective scale=%u; fixed12x7 tiles, tile=%u\n",requested,scale,scale*32);
        s.costs.requested_scale=requested;s.costs.effective_scale=scale;
    }
    s.costs.last_upload_bytes=0;int timing=s.begin_time();
    struct TimeEnd {Impl& s;int i;bool good=false;~TimeEnd(){s.end_time(i,good);}} timer{s,timing};
    if(!s.tile(scale) || !s.upload(frame)){s.failed=true;return false;}
    int native_height=stereo?448:224;
    double fit=std::min(double(width)/384,double(height)/native_height);
    int dw=int(384*fit),dh=int(native_height*fit),left=(width-dw)/2,top=(height-dh)/2;
    s.gl.BindFramebuffer(GL_FRAMEBUFFER,0);s.gl.Viewport(0,0,width,height);s.gl.ClearColor(0,0,0,1);s.gl.Clear(GL_COLOR_BUFFER_BIT);
    for(int eye=0;eye<(stereo?2:1);++eye) {
        if(!s.eye_jobs(frame,eye)){s.failed=true;return false;}
        // Fixed logical12x7 tile grid: CPU jobs, uploads and submission count
        // do not grow with scale. Only GPU tile dimensions are32*scale.
        for(unsigned ty=0;ty<7;++ty)for(unsigned tx=0;tx<12;++tx) {
            s.draw_tile(tx,ty,scale,frame.counts[eye]);
            int x0=left+int(tx*32)*dw/384,x1=left+int((tx+1)*32)*dw/384;
            int y0=top+int(eye*224+ty*32)*dh/native_height,y1=top+int(eye*224+(ty+1)*32)*dh/native_height;
            s.gl.BindFramebuffer(GL_READ_FRAMEBUFFER,s.fbo);s.gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
            s.gl.BlitFramebuffer(0,0,s.tile_size,s.tile_size,x0,height-y0,x1,height-y1,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        }
    }
    if(s.gl.GetError()!=GL_NO_ERROR){s.failed=true;return false;}
    timer.good=true;++s.costs.frames;s.costs.max_upload_bytes=std::max(s.costs.max_upload_bytes,s.costs.last_upload_bytes);
    s.end_time(timer.i,true);timer.i=-1;SDL_GL_SwapWindow(s.window);return true;
}
bool VbVipGpuRaster::readback_native(const VbVipGpuFrame& frame,int eye,uint32_t* out) {
    if(!impl_ || !out || (unsigned)eye>1)return false;
    auto& s=*impl_;Context use(s.window,s.context);if(!use.good || !s.tile(1) || !s.upload(frame) || !s.eye_jobs(frame,eye))return false;
    uint32_t tile[32*32];
    for(unsigned ty=0;ty<7;++ty)for(unsigned tx=0;tx<12;++tx) {
        s.draw_tile(tx,ty,1,frame.counts[eye]);s.gl.ReadPixels(0,0,32,32,GL_BGRA,GL_UNSIGNED_INT_8_8_8_8_REV,tile);
        for(unsigned y=0;y<32;++y)std::memcpy(out+(ty*32+y)*384+tx*32,tile+y*32,32*sizeof(uint32_t));
    }
    return s.gl.GetError()==GL_NO_ERROR;
}
bool VbVipGpuRaster::present_current(SDL_Window* window,int slot,bool stereo,unsigned scale,int width,int height,bool vsync) {
    if(!initialize(window))return false;
    auto& s=*impl_;VbVipGpuFrame frame{};uint32_t bitmap_rows=vb_vip_raster_bitmap_rows(slot);
    for(int row=0;row<28;++row)if(!(bitmap_rows&(1u<<row)) && !vb_viewport_raster_bank(slot,row,&frame.banks[row]))return false;
    for(int eye=0;eye<(stereo?2:1);++eye) {
        if(s.jobs[eye].size()!=VB_VIP_RASTER_JOB_LIMIT)s.jobs[eye].resize(VB_VIP_RASTER_JOB_LIMIT);
        if(!vb_vip_raster_prepare(frame.banks,eye,bitmap_rows,s.jobs[eye].data(),VB_VIP_RASTER_JOB_LIMIT,&frame.counts[eye]))return false;
        frame.jobs[eye]=s.jobs[eye].data();vb_vip_column_argb(eye,frame.colors[eye]);
        if(bitmap_rows)frame.bitmaps[eye]=vb_vip_raster_bitmap(slot,eye,&frame.bitmap_revision[eye]);
    }
    return present(frame,stereo,scale,width,height,vsync);
}
VbVipGpuRasterCosts VbVipGpuRaster::costs(){
    VbVipGpuRasterCosts out{};if(impl_){Context use(impl_->window,impl_->context);if(use.good)impl_->collect_times();out=impl_->costs;}out.init_failures=init_failures_;return out;
}
void VbVipGpuRaster::reset(){impl_.reset();attempted_=false;attempted_window_=nullptr;init_failures_=0;}
#else
struct VbVipGpuRaster::Impl{};
VbVipGpuRaster::VbVipGpuRaster()=default;VbVipGpuRaster::~VbVipGpuRaster()=default;
bool VbVipGpuRaster::initialize(SDL_Window*){return false;}
bool VbVipGpuRaster::present_current(SDL_Window*,int,bool,unsigned,int,int,bool){return false;}
bool VbVipGpuRaster::present(const VbVipGpuFrame&,bool,unsigned,int,int,bool){return false;}
bool VbVipGpuRaster::readback_native(const VbVipGpuFrame&,int,uint32_t*){return false;}
VbVipGpuRasterCosts VbVipGpuRaster::costs(){return {};}
void VbVipGpuRaster::reset(){}
#endif
