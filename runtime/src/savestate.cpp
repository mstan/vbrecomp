#include "savestate.h"
#include "memory.h"
#include "machine.h"
#include "ring_frame.h"
#include "renderer.h"
#include "viewport.h"
#include "vb_crc32.h"
#include "vb_sha256.h"
#include "retcomm_rbengine/snap_ring.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <limits>
#include <new>
#include <memory>
#include <filesystem>
#include <array>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#ifndef VB_STATE_BUILD_ID
#error Snapshot build identity must be supplied by CMake
#endif
namespace {
CPUState* cpu;
uint32_t* dispatch_pc;
uint64_t* frame_cycles;
void (*lock_audio)(void*);
void (*unlock_audio)(void*);
void* audio_context;
unsigned audio_guard_depth;
const uint8_t* previous_sram;
uint64_t generation;
uint8_t content[32];
RbeSnapRing* rewind_ring;
struct Extension { char id[96]; void* bytes; size_t size; };
Extension extensions[16];
unsigned extension_count;
struct SimulationExtension {
    Extension span;
    VbStateValidate validate;
    VbStateLoaded loaded;
    void* context;
};
SimulationExtension simulation_extensions[16];
unsigned simulation_extension_count;
uint8_t* reset_blob;
size_t reset_size;
const char* error = "";
constexpr size_t header_size = 112, max_size = 64u * 1024u * 1024u;
struct Stream {
    uint8_t* out;
    const uint8_t* in;
    size_t offset;
    const uint8_t* inspect = nullptr;
    bool valid = true;
};
void field(void* ctx, void* bytes, size_t size) {
    auto& s = *static_cast<Stream*>(ctx);
    if (s.out) {
        std::memcpy(s.out + s.offset, bytes, size);
    } else if (s.in) std::memcpy(bytes, s.in + s.offset, size);
    s.offset += size;
}
void visit(Stream& s, bool presentation) {
    field(&s, cpu, offsetof(CPUState, read8)); // exclude all host bus pointers
    field(&s, dispatch_pc, sizeof(*dispatch_pc));
    field(&s, frame_cycles, sizeof(*frame_cycles));
    vb_memory_state_visit(field, &s); vb_irq_state_visit(field, &s);
    vb_timer_state_visit(field, &s); vb_input_state_visit(field, &s);
    vb_vip_state_visit(field, &s); vb_vsu_state_visit(field, &s);
    vb_vsu_shadow_state_visit(field, &s);
    for(unsigned i=0;i<simulation_extension_count;++i) {
        const auto& e=simulation_extensions[i];
        if(s.inspect && !e.validate(s.inspect+s.offset,e.span.size,e.context))s.valid=false;
        field(&s,e.span.bytes,e.span.size);
    }
    if (presentation) {
        vb_vip_presentation_visit(field, &s); vb_viewport_state_visit(field, &s);
        for(unsigned i=0;i<extension_count;++i)field(&s,extensions[i].bytes,extensions[i].size);
    }
}
void put32(uint8_t* p, uint32_t n) { for (unsigned i=0;i<4;++i) p[i]=(uint8_t)(n>>(i*8)); }
uint32_t get32(const uint8_t* p) {
    uint32_t n=0; for(unsigned i=0;i<4;++i) n|=(uint32_t)p[i]<<(i*8); return n;
}
void build_hash(uint8_t out[32]) {
    vb_sha256_compute(reinterpret_cast<const uint8_t*>(VB_STATE_BUILD_ID), std::strlen(VB_STATE_BUILD_ID), out);
}
void feature_hash(uint8_t out[32]) {
    std::vector<uint8_t> manifest;
    const char* ids[]={vb_renderer_id(),vb_viewport_id()};
    for(const char* id:ids) {
        const size_t n=std::strlen(id)+1;
        manifest.insert(manifest.end(),id,id+n);
    }
    for(unsigned i=0;i<extension_count;++i) {
        const auto& e=extensions[i];
        manifest.insert(manifest.end(),e.id,e.id+std::strlen(e.id)+1);
        uint8_t n[8];put32(n,(uint32_t)e.size);put32(n+4,(uint32_t)((uint64_t)e.size>>32));
        manifest.insert(manifest.end(),n,n+8);
    }
    for(unsigned i=0;i<simulation_extension_count;++i) {
        const auto& e=simulation_extensions[i].span;
        constexpr char kind[]="simulation";
        manifest.insert(manifest.end(),kind,kind+sizeof(kind));
        manifest.insert(manifest.end(),e.id,e.id+std::strlen(e.id)+1);
        uint8_t n[8];put32(n,(uint32_t)e.size);put32(n+4,(uint32_t)((uint64_t)e.size>>32));
        manifest.insert(manifest.end(),n,n+8);
    }
    vb_sha256_compute(manifest.data(),manifest.size(),out);
}
bool valid() {
    if (!cpu || cpu->gpr[0] || (cpu->pc & 1) || (*dispatch_pc & 1)
        || cpu->halted > 1 || cpu->yielded > 1 || cpu->bstr_src_valid > 1
        || cpu->pipeline_class < -1 || cpu->pipeline_class > 4 || cpu->bus_tail_cycles > 64
        || cpu->psw_int_level > 15 || cpu->cycles < *frame_cycles
        || (cpu->sysreg[VB_SR_CHCW]&~2u)
        || cpu->cycles - *frame_cycles >= 397824u
        || *frame_cycles != (uint64_t)cpu->frame * 397824u) return false;
    const uint8_t* flags=&cpu->psw_z;
    for(unsigned i=0;i<14;++i) if(flags[i]>1) return false;
    return vb_vip_state_valid() && vb_vsu_state_valid()
        && vb_timer_state_valid() && vb_input_state_valid() && vb_vsu_shadow_state_valid()
        && vb_memory_state_valid() && vb_irq_state_valid();
}
struct AudioGuard {
    // All callers are on the guest thread. A game may cold-reset at a field
    // boundary inside deterministic replay; audio exclusion must nest there.
    AudioGuard() { if(audio_guard_depth++==0 && lock_audio) lock_audio(audio_context); }
    ~AudioGuard() { if(--audio_guard_depth==0 && unlock_audio) unlock_audio(audio_context); }
};
}
extern "C" {
static bool extension_id_available(const char* id) {
    if(!id || !*id || std::strlen(id)>=96)return false;
    for(unsigned i=0;i<extension_count;++i)if(!std::strcmp(id,extensions[i].id))return false;
    for(unsigned i=0;i<simulation_extension_count;++i)
        if(!std::strcmp(id,simulation_extensions[i].span.id))return false;
    return true;
}
int vb_state_register_presentation(const char* id, void* bytes, size_t size) {
    if(cpu || !extension_id_available(id) || !bytes || !size
       || size>1024u*1024u || extension_count>=16)return 0;
    auto& e=extensions[extension_count++];std::strcpy(e.id,id);e.bytes=bytes;e.size=size;return 1;
}
int vb_state_register_simulation(const char* id, void* bytes, size_t size,
                                 VbStateValidate validate, VbStateLoaded loaded,
                                 void* context) {
    if(cpu || !extension_id_available(id) || !bytes || !size || !validate
       || size>1024u*1024u || simulation_extension_count>=16
       || !validate(bytes,size,context))return 0;
    auto& e=simulation_extensions[simulation_extension_count++];
    std::strcpy(e.span.id,id);e.span.bytes=bytes;e.span.size=size;
    e.validate=validate;e.loaded=loaded;e.context=context;return 1;
}
void vb_state_bind(CPUState* c, uint32_t* pc, uint64_t* cycles,
                   void (*lock)(void*), void (*unlock)(void*), void* audio) {
    if(!vb_rom_size()) {cpu=nullptr;return;}
    cpu=c; dispatch_pc=pc; frame_cycles=cycles;
    lock_audio=lock; unlock_audio=unlock; audio_context=audio;
    vb_sha256_compute(vb_rom_data(), vb_rom_size(), content);
    std::free(reset_blob);reset_blob=nullptr;reset_size=0;
    vb_state_capture(&reset_blob,&reset_size,1);
}
const char* vb_state_error(void) { return error; }
uint64_t vb_state_generation(void) { return generation; }
const uint8_t* vb_state_previous_sram(void){return previous_sram;}
const char* vb_state_build_id(void) { return VB_STATE_BUILD_ID; }
const uint8_t* vb_state_content_id(void) { return content; }
/* Private memory snapshots are owned immutable blobs, never file input. Their
 * integrity comes from ownership; only the strict public file/blob interface
 * accepts external data and requires a payload CRC. */
static int capture_impl(uint8_t** data, size_t* size, int presentation, bool trusted) {
    if (!cpu || !data || !size) { error="Snapshot host is not bound"; return 0; }
    *data=nullptr; *size=0;
    try {
        // The guest thread owns all visited data. The audio consumer touches
        // only the excluded playback cursor, so capture needs no audio lock.
        Stream count{nullptr,nullptr,header_size}; visit(count,presentation!=0);
        if(count.offset>max_size) {error="Snapshot exceeds limit";return 0;}
        auto allocated=static_cast<uint8_t*>(std::malloc(count.offset));
        std::unique_ptr<uint8_t,decltype(&std::free)> owner(allocated,&std::free);
        if(!allocated) {error="Snapshot allocation failed";return 0;}
        std::memset(allocated,0,header_size);
        Stream output{allocated,nullptr,header_size};visit(output,presentation!=0);
        std::memcpy(allocated,"VBSTATE1",8);
        put32(allocated+8,(uint32_t)count.offset);
        put32(allocated+12,(presentation?1u:0u)|(trusted?2u:0u));
        build_hash(allocated+16);std::memcpy(allocated+48,content,32);
        if(!trusted)put32(allocated+80,vb_crc32_compute(allocated+header_size,count.offset-header_size));
        put32(allocated+84,0x01020304u);
        uint8_t features[32];feature_hash(features);std::memcpy(allocated+88,features,24);
        *data=owner.release();*size=count.offset;error="";return 1;
    } catch(const std::bad_alloc&) {error="Snapshot allocation failed";return 0;}
}
int vb_state_capture(uint8_t** data, size_t* size, int presentation) {
    return capture_impl(data,size,presentation,false);
}
static int restore_impl(const uint8_t* data, size_t size, bool trusted, bool keep_history,
                        bool inspect_only=false,int (*policy)(void*)=nullptr,void* context=nullptr) {
    uint8_t build[32];build_hash(build);
    uint8_t features[32];feature_hash(features);
    if(!cpu || !data || size<header_size || size>max_size || std::memcmp(data,"VBSTATE1",8)
       || get32(data+8)!=size || get32(data+12)!=(trusted?3u:1u) || std::memcmp(data+16,build,32)
       || std::memcmp(data+48,content,32) || get32(data+84)!=0x01020304u
       || std::memcmp(data+88,features,24)
       || (!trusted && get32(data+80)!=vb_crc32_compute(data+header_size,size-header_size))) {
        error="Invalid, damaged, different-content or different-build snapshot";return 0;
    }
    Stream count{nullptr,nullptr,header_size};visit(count,true);
    if(count.offset!=size) {error="Snapshot layout does not match active features";return 0;}
    Stream inspect{nullptr,nullptr,header_size,data};visit(inspect,true);
    if(!inspect.valid) {error="Snapshot contains invalid game simulation state";return 0;}
    // Keep the original state until every range validator accepts the loaded
    // scalars. No guest instruction, device tick or output callback runs here.
    uint8_t* backup=nullptr;size_t backup_size=0;
    if(!capture_impl(&backup,&backup_size,1,true))return 0;
    AudioGuard guard;
    std::array<uint8_t,65536> old_sram;
    std::memcpy(old_sram.data(),vb_cart_ram_data(),old_sram.size());
    Stream load{nullptr,data,header_size};visit(load,true);
    bool accepted=false;
    try {accepted=valid() && (!policy || policy(context));}
    catch(...) {
        Stream undo{nullptr,backup,header_size};visit(undo,true);std::free(backup);throw;
    }
    if(!accepted || inspect_only) {
        Stream undo{nullptr,backup,header_size};visit(undo,true);std::free(backup);
        error=accepted?"":"Snapshot contains invalid device, scheduler or policy state";return accepted?1:0;
    }
    std::free(backup);vb_vsu_state_loaded();vb_vip_state_loaded();++generation;
    if(!keep_history)rbe_snap_ring_clear(rewind_ring);
    previous_sram=old_sram.data();
    for(unsigned i=0;i<simulation_extension_count;++i) {
        const auto& e=simulation_extensions[i];if(e.loaded)e.loaded(e.context);
    }
    previous_sram=nullptr;
    error="";return 1;
}
int vb_state_restore(const uint8_t* data, size_t size) {
    try { return restore_impl(data,size,false,false); }
    catch(const std::bad_alloc&) {error="Snapshot allocation failed";return 0;}
}
int vb_state_validate(const uint8_t* data,size_t size,int (*policy)(void*),void* context) {
    try {return restore_impl(data,size,false,true,true,policy,context);}
    catch(const std::bad_alloc&) {error="Snapshot allocation failed";return 0;}
}
int vb_state_save_file(const char* path) {
    uint8_t* blob=nullptr;size_t size=0;
    if(!path || !vb_state_capture(&blob,&size,1))return 0;
    std::unique_ptr<uint8_t,decltype(&std::free)> owner(blob,&std::free);
    try {
    const auto destination=std::filesystem::u8path(path);
    auto temporary=destination;temporary+=".tmp";
#ifdef _WIN32
    FILE* f=_wfopen(temporary.c_str(),L"wb");
#else
    FILE* f=std::fopen(temporary.c_str(),"wb");
#endif
    if(!f){error="Cannot open snapshot file";return 0;}
    bool ok=std::fwrite(blob,1,size,f)==size;ok=std::fclose(f)==0 && ok;
    if(ok) {
#ifdef _WIN32
        ok=MoveFileExW(temporary.c_str(),destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
#else
        std::error_code ec;std::filesystem::rename(temporary,destination,ec);ok=!ec;
#endif
    }
    error=ok?"":"Cannot write snapshot file";return ok;
    } catch(const std::filesystem::filesystem_error&) {error="Invalid snapshot path";return 0;}
      catch(const std::bad_alloc&) {error="Snapshot allocation failed";return 0;}
}
int vb_state_load_file(const char* path) {
    if(!path)return 0;
    try {
#ifdef _WIN32
    FILE* f=_wfopen(std::filesystem::u8path(path).c_str(),L"rb");
#else
    FILE* f=std::fopen(path,"rb");
#endif
    if(!f){error="Cannot open snapshot file";return 0;}
    std::unique_ptr<FILE,decltype(&std::fclose)> file(f,&std::fclose);
    if(std::fseek(f,0,SEEK_END) || std::ftell(f)<0){error="Cannot read snapshot size";return 0;}
    const long n=std::ftell(f);
    if(n<(long)header_size || (size_t)n>max_size){error="Invalid snapshot size";return 0;}
    std::rewind(f);
        std::vector<uint8_t> blob((size_t)n);bool ok=std::fread(blob.data(),1,blob.size(),f)==blob.size();
        if(!ok){error="Cannot read snapshot file";return 0;}
        return vb_state_restore(blob.data(),blob.size());
    } catch(const std::filesystem::filesystem_error&) {error="Invalid snapshot path";return 0;}
      catch(const std::bad_alloc&) {error="Snapshot allocation failed";return 0;}
}
int vb_state_reset(void) {
    if(!reset_blob)return 0;
    uint8_t sram[65536];std::memcpy(sram,vb_cart_ram_data(),sizeof(sram));
    if(!vb_state_restore(reset_blob,reset_size))return 0;
    std::memcpy(vb_cart_ram_data(),sram,sizeof(sram));
    rbe_snap_ring_clear(rewind_ring);return 1;
}
int vb_state_replay_frames(unsigned frames) {
    if(!cpu || !frames || frames>10000 || frames>UINT32_MAX-cpu->frame)return 0;
    AudioGuard guard;
    for(unsigned i=0;i<frames;++i) {
        if(!vb_machine_run_frame(cpu,dispatch_pc,frame_cycles)) {
            error="Debugger stopped deterministic replay";++generation;return 0;
        }
        vb_ring_frame_record(cpu);
    }
    vb_vsu_discard_playback();++generation;error="";return 1;
}
void vb_state_shutdown(void) {
    vb_rewind_shutdown();std::free(reset_blob);reset_blob=nullptr;reset_size=0;cpu=nullptr;
}
uint32_t vb_state_digest(void) {
    uint8_t* blob=nullptr;size_t size=0;if(!vb_state_capture(&blob,&size,0))return 0;
    uint32_t hash=get32(blob+80);std::free(blob);return hash;
}
int vb_rewind_enable(unsigned depth) {
    if(depth<2 || depth>512)return 0;
    uint8_t* sample=nullptr;size_t size=0;
    if(!vb_state_capture(&sample,&size,1))return 0;
    std::free(sample);
    if(size>512u*1024u*1024u/depth) {
        error="Rewind history exceeds 512 MiB; reduce the frame count";return 0;
    }
    RbeSnapRing* next=rbe_snap_ring_create(depth);if(!next)return 0;
    vb_rewind_shutdown();rewind_ring=next;return 1;
}
void vb_rewind_shutdown(void){rbe_snap_ring_destroy(rewind_ring);rewind_ring=nullptr;}
int vb_rewind_record(uint32_t tick) {
    if(!rewind_ring)return 1;
    uint8_t* blob=nullptr;size_t size=0;if(!capture_impl(&blob,&size,1,true))return 0;
    if(!rbe_snap_ring_store(rewind_ring,tick,blob,size)){std::free(blob);return 0;}return 1;
}
int vb_rewind_to(uint32_t tick) {
    if(!rewind_ring){error="Rewind is disabled";return 0;}
    size_t size=0;const uint8_t* blob=rbe_snap_ring_peek(rewind_ring,tick,&size);
    if(!blob){error="Requested rewind frame is no longer available";return 0;}
    if(!restore_impl(blob,size,true,true))return 0;
    rbe_snap_ring_drop_after(rewind_ring,tick);return 1;
}
unsigned vb_rewind_count(void){return rbe_snap_ring_count(rewind_ring);}
int vb_rewind_has(uint32_t tick){return rbe_snap_ring_has(rewind_ring,tick);}
int vb_rewind_enabled(void){return rewind_ring!=nullptr;}
struct VbRollbackHistory {RbeSnapRing* ring;};
VbRollbackHistory* vb_rollback_history_create(unsigned depth) {
    if(depth<2 || depth>512)return nullptr;
    uint8_t* sample=nullptr;size_t size=0;
    if(!capture_impl(&sample,&size,1,true))return nullptr;
    std::free(sample);
    if(size>512u*1024u*1024u/depth){error="Rollback history exceeds 512 MiB";return nullptr;}
    auto* history=new(std::nothrow) VbRollbackHistory{rbe_snap_ring_create(depth)};
    if(history && !history->ring){delete history;return nullptr;}return history;
}
void vb_rollback_history_destroy(VbRollbackHistory* h) {
    if(h){rbe_snap_ring_destroy(h->ring);delete h;}
}
int vb_rollback_history_save(VbRollbackHistory* h,uint32_t tick) {
    if(!h)return 0;
    uint8_t* blob=nullptr;size_t size=0;if(!capture_impl(&blob,&size,1,true))return 0;
    if(!rbe_snap_ring_store(h->ring,tick,blob,size)){std::free(blob);return 0;}return 1;
}
int vb_rollback_history_load(VbRollbackHistory* h,uint32_t tick) {
    if(!h)return 0;
    size_t size=0;const auto* blob=rbe_snap_ring_peek(h->ring,tick,&size);
    return blob && restore_impl(blob,size,true,true);
}
int vb_rollback_history_has(const VbRollbackHistory* h,uint32_t tick){return h && rbe_snap_ring_has(h->ring,tick);}
int vb_rollback_history_oldest(const VbRollbackHistory* h,uint32_t* tick) {
    if(!h || !tick || !rbe_snap_ring_count(h->ring))return 0;
    *tick=rbe_snap_ring_oldest_tick(h->ring);return 1;
}
void vb_rollback_history_drop_after(VbRollbackHistory* h,uint32_t tick){if(h)rbe_snap_ring_drop_after(h->ring,tick);}
}
