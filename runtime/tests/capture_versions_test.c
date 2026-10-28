/* Exercise the real canonical VIP bus and the frozen viewport bank rotation. */
#include <assert.h>
#include "../src/vip.c"
int vb_renderer_tracks_texels(void){return 0;}
int vb_renderer_tracks_worlds(void){return 0;}
int vb_renderer_active(void){return 0;}
uint32_t vb_capture_hash(const uint16_t data[8]){
    uint32_t h=2166136261u;const uint8_t* p=(const uint8_t*)data;
    for(unsigned i=0;i<16;++i)h=(h^p[i])*16777619u;return h;
}
static void capture(void) {
    vb_viewport_capture(0,0,dram_u16(),chr_u16(),s_spt,s_gplt_cache,s_jplt_cache,s_bkcol,s_obj_suppress);
}
static uint8_t state_bytes[16*1024*1024];
typedef struct StateCheck {size_t offset;int compare;unsigned qualified;} StateCheck;
static void state_field(void* context,void* data,size_t size) {
    StateCheck* c=(StateCheck*)context;assert(size<=sizeof(state_bytes)-c->offset);
    if(c->compare)assert(!memcmp(state_bytes+c->offset,data,size));else memcpy(state_bytes+c->offset,data,size);
    c->offset+=size;
}
static void frozen_field(void* context,const void* data,size_t size,uint64_t token) {
    StateCheck* c=(StateCheck*)context;assert(size==4096 && token);++c->qualified;
    state_field(context,(void*)data,size);
}
static unsigned check_state_pages(void) {
    StateCheck original={0},qualified={0};vb_viewport_state_visit(state_field,&original);
    qualified.compare=1;vb_viewport_state_visit_pages(state_field,frozen_field,&qualified);
    assert(original.offset==qualified.offset);return qualified.qualified;
}
static void check_capture(void) {
    VbVipDrawInputs b;assert(vb_viewport_draw_inputs(0,0,&b));
    assert(!memcmp(b.dram,dram_u16(),0x20000));assert(!memcmp(b.chars,chr_u16(),0x8000));
    assert(!memcmp(b.spt,s_spt,sizeof(s_spt)));assert(!memcmp(b.gplt,s_gplt_cache,sizeof(s_gplt_cache)));
    for(unsigned tile=0;tile<2048;++tile){
        uint32_t h;assert(vb_viewport_tile_hash(b.chars,tile,&h));assert(h==vb_capture_hash(b.chars+tile*8));
        assert(vb_viewport_tile_hash(b.chars,tile,&h));assert(h==vb_capture_hash(b.chars+tile*8));
    }
}
int main(void) {
    static uint8_t saved_memory[sizeof(s_vip_mem)],old_dram[0x20000],old_chars[0x8000];
    static uint16_t external_dram[65536],external_chars[16384];
    vb_viewport_track();memset(s_vip_mem,0x29,sizeof(s_vip_mem));capture_versions_reset();
    uint64_t c0,r0,c1,r1;vb_viewport_capture_costs(&c0,&r0);
    capture();check_capture();vb_viewport_capture_costs(&c1,&r1);assert(check_state_pages()==40);
    assert(c1-c0==0x28000 && r1==r0);
    c0=c1;r0=r1;capture();check_capture();vb_viewport_capture_costs(&c1,&r1);
    assert(c1==c0 && r1-r0==40);
    for(unsigned page=0;page<40;++page){
        unsigned at=page<32 ? 0x20000+page*4096:VIP_CHR_CANONICAL_BASE+(page-32)*4096;
        vb_vip_write8(at+4095,(uint8_t)page);
        c0=c1;r0=r1;capture();check_capture();vb_viewport_capture_costs(&c1,&r1);
        assert(c1-c0==4096 && r1-r0==39);
        vb_vip_write16(at+7,(uint16_t)(page*17));
        vb_vip_write32(at+4095,0x12345678u+page);capture();check_capture();
        vb_viewport_capture_costs(&c1,&r1);
    }
    /* The four noncontiguous CHR aliases must update canonical page revisions. */
    for(unsigned bank=0;bank<4;++bank){vb_vip_write32(0x6000+bank*0x8000,0xF00D0000u+bank);capture();check_capture();}
    VbVipDrawInputs old;assert(vb_viewport_draw_inputs(0,0,&old));
    memcpy(old_dram,old.dram,sizeof(old_dram));memcpy(old_chars,old.chars,sizeof(old_chars));
    vb_vip_native_rows[0]=1;vb_vip_native_pending=1;
    vb_vip_write16(0x20024,0xBEEF);vb_vip_write16(0x7800A,0x1234);capture();check_capture();
    assert(!memcmp(old.dram,old_dram,sizeof(old_dram)));assert(!memcmp(old.chars,old_chars,sizeof(old_chars)));
    for(unsigned tile=0;tile<2048;++tile){uint32_t h;assert(vb_viewport_tile_hash(old.chars,tile,&h));assert(h==vb_capture_hash(old.chars+tile*8));}
    vb_vip_native_rows[0]=vb_vip_native_pending=0;
    /* Unsupported/external pointers always copy, and invalidate that bank's key. */
    for(unsigned i=0;i<2;++i){
        external_dram[3]=(uint16_t)i;
        vb_viewport_capture(0,0,external_dram,external_chars,s_spt,s_gplt_cache,s_jplt_cache,1,s_obj_suppress);
        VbVipDrawInputs b;assert(vb_viewport_draw_inputs(0,0,&b));assert(b.dram[3]==i);
        assert(check_state_pages()==0);
    }
    capture();check_capture();
    memcpy(saved_memory,s_vip_mem,sizeof(saved_memory));
    vb_vip_write32(0x20030,0xAABBCCDD);capture();check_capture();
    const uint64_t preceding_token=capture_revision_clock;
    memcpy(s_vip_mem,saved_memory,sizeof(saved_memory));vb_vip_state_loaded();assert(capture_revision_clock>preceding_token);
    assert(check_state_pages()==0);capture();check_capture();assert(check_state_pages()==40);
    capture_revision_clock=UINT64_MAX;vb_vip_write16(0x78000,0x9876);capture();check_capture();
    assert(capture_revision_exhausted && !vb_vip_capture_revisions(dram_u16(),chr_u16()));
    assert(check_state_pages()==0);
    uint32_t h;assert(!vb_viewport_tile_hash(chr_u16(),0,&h));
    assert(!vb_viewport_tile_hash(old.chars,2048,&h));assert(!vb_viewport_tile_hash(old.chars,0,NULL));
    return 0;
}
