/* Exercise the real canonical VIP bus and the frozen viewport bank rotation. */
#include <assert.h>
#include "../src/vip.c"
int vb_renderer_tracks_texels(void){return 0;}
int vb_renderer_tracks_worlds(void){return 0;}
int vb_renderer_active(void){return 0;}
uint32_t vb_capture_hash(const uint16_t data[8]){(void)data;assert(0);return 0;}
static void capture(void) {
    vb_viewport_capture(0,0,dram_u16(),chr_u16(),s_spt,s_gplt_cache,s_jplt_cache,s_bkcol,s_obj_suppress);
}
static void check_capture(void) {
    VbVipDrawInputs b;assert(vb_viewport_draw_inputs(0,0,&b));
    assert(!memcmp(b.dram,dram_u16(),0x20000));assert(!memcmp(b.chars,chr_u16(),0x8000));
    assert(!memcmp(b.spt,s_spt,sizeof(s_spt)));assert(!memcmp(b.gplt,s_gplt_cache,sizeof(s_gplt_cache)));
}
int main(void) {
    static uint8_t saved_memory[sizeof(s_vip_mem)],old_dram[0x20000],old_chars[0x8000];
    static uint16_t external_dram[65536],external_chars[16384];
    vb_viewport_track();memset(s_vip_mem,0x29,sizeof(s_vip_mem));capture_versions_reset();
    uint64_t c0,r0,c1,r1;vb_viewport_capture_costs(&c0,&r0);
    capture();check_capture();vb_viewport_capture_costs(&c1,&r1);
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
    vb_vip_native_rows[0]=vb_vip_native_pending=0;
    /* Unsupported/external pointers always copy, and invalidate that bank's key. */
    for(unsigned i=0;i<2;++i){
        external_dram[3]=(uint16_t)i;
        vb_viewport_capture(0,0,external_dram,external_chars,s_spt,s_gplt_cache,s_jplt_cache,1,s_obj_suppress);
        VbVipDrawInputs b;assert(vb_viewport_draw_inputs(0,0,&b));assert(b.dram[3]==i);
    }
    capture();check_capture();
    memcpy(saved_memory,s_vip_mem,sizeof(saved_memory));
    vb_vip_write32(0x20030,0xAABBCCDD);capture();check_capture();
    memcpy(s_vip_mem,saved_memory,sizeof(saved_memory));vb_vip_state_loaded();capture();check_capture();
    capture_revision_clock=UINT64_MAX;vb_vip_write16(0x78000,0x9876);capture();check_capture();
    assert(capture_revision_clock==1);
    return 0;
}
