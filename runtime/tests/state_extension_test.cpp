#include "savestate.h"
#include "memory.h"
#include "vb_crc32.h"
#include "renderer.h"
#include "viewport.h"
#include "state_ui.h"
#include "vip.h"
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <vector>

namespace {
struct Session {uint32_t actors,tick,values[4];};
Session session{};
uint64_t presentation=123;
unsigned loaded;
unsigned old_sram_value;
bool draw_observer_active;
unsigned draw_observed;
uint32_t draw_observer(const CPUState* cpu,uint32_t addr,uint32_t value,unsigned width) {
    if(!draw_observer_active)return 0;
    assert(cpu && cpu->gpr[7]==77 && value==0x03030303 && width==4);
    unsigned slot=(addr>>15)&1;
    VbVipDrawCosts costs{};vb_vip_draw_costs(&costs);
    assert(!(costs.pending[slot]&3)); // Native rows exist before the observer.
    ++draw_observed;return 777;
}
void deferred_bus_test(CPUState& cpu) {
    assert(vb_memory_register_write_observer(draw_observer));
    vb_vip_write16(0x3dbe0,0x40); // End world list: backdrop-only native rows.
    vb_vip_write16(0x5f870,2);vb_vip_write16(0x5f842,2);
    VbVipDrawCosts costs{};unsigned slot=0;
    for(unsigned i=0;i<2000;++i) {
        vb_vip_tick(4480);vb_vip_draw_costs(&costs);
        if((costs.pending[0]&3)==3){slot=0;break;}
        if((costs.pending[1]&3)==3){slot=1;break;}
    }
    assert((costs.pending[slot]&3)==3);
    cpu.gpr[7]=77;draw_observer_active=true;
    vb_write32(0x800000u+slot*0x8000u,0x03030303);
    draw_observer_active=false;assert(draw_observed==1);
    if(unsigned(vb_vip_display_fb())!=slot)vb_vip_write16(0x5f842,1);
    const auto* sources=vb_vip_source_buffer(0);
    assert(sources[0].kind==4 && sources[0].tile_hash==777 && sources[0].raw==3);
    vb_vip_init();
    std::puts("PASS: real bus flushes deferred rows before write observer and CPU provenance");
}
int validate(const void* data,size_t size,void*) {
    if(size!=sizeof(Session))return 0;
    Session copy;std::memcpy(&copy,data,size);return copy.actors<=4;
}
void did_load(void*){++loaded;assert(vb_state_previous_sram());old_sram_value=vb_state_previous_sram()[999];}
std::vector<uint8_t> capture(int view=1) {
    uint8_t* data=nullptr;size_t size=0;assert(vb_state_capture(&data,&size,view));
    std::vector<uint8_t> result(data,data+size);std::free(data);return result;
}
void put32(uint8_t* p,uint32_t value){for(unsigned i=0;i<4;++i)p[i]=(uint8_t)(value>>(8*i));}
void crc(std::vector<uint8_t>& blob){put32(blob.data()+80,vb_crc32_compute(blob.data()+112,blob.size()-112));}
}
int main(int argc,char** argv) {
    assert(argc==2 || (argc==3 && !std::strcmp(argv[2],"--wide-history")));
    const bool wide_history=argc==3;
    if(wide_history) {vb_renderer_track_texels();vb_viewport_track();}
    assert(vb_state_register_simulation("test.session.v1",&session,sizeof(session),validate,did_load,nullptr));
    assert(!vb_state_register_presentation("test.session.v1",&presentation,sizeof(presentation)));
    assert(!vb_state_register_simulation("test.no-validator",&session,sizeof(session),nullptr,nullptr,nullptr));
    assert(vb_state_register_presentation("test.presentation.v1",&presentation,sizeof(presentation)));
    assert(!vb_state_register_simulation("test.presentation.v1",&session,sizeof(session),validate,nullptr,nullptr));
    assert(vb_memory_init(argv[1])==0);
    CPUState cpu{};cpu.pc=VB_RESET_VECTOR;vb_psw_unpack(&cpu,VB_PSW_RESET);
    cpu.read8=vb_read8;cpu.read16=vb_read16;cpu.read32=vb_read32;
    cpu.write8=vb_write8;cpu.write16=vb_write16;cpu.write32=vb_write32;
    vb_memory_set_cpu(&cpu);uint32_t pc=cpu.pc;uint64_t frame_cycles=0;
    vb_state_bind(&cpu,&pc,&frame_cycles,nullptr,nullptr,nullptr);
    if(wide_history) {
        deferred_bus_test(cpu);
        const auto wide=capture();
        assert(wide.size()>16u*1024u*1024u);
        assert(vb_rewind_enable(32));
        session.tick=17;assert(vb_rewind_record(10));
        session.tick=29;assert(vb_rewind_record(20));
        assert(vb_rewind_to(10) && session.tick==17);
        assert(!vb_rewind_has(20));
        const auto revision=vb_rewind_revision();assert(!vb_rewind_enable(512));
        assert(vb_rewind_revision()==revision);
        assert(vb_rewind_has(10)); // Rejection preserves existing history.
        std::printf("wide snapshot: %zu bytes; 32 entries: %zu bytes\n",wide.size(),wide.size()*32);
        vb_state_shutdown();vb_memory_shutdown();return 0;
    }
    assert(!vb_state_register_simulation("test.too-late",&session,sizeof(session),validate,nullptr,nullptr));
    const auto reset=capture();
    const auto initial_digest=vb_state_digest();
    session={4,17,{10,20,30,40}};
    const auto canonical_digest=vb_state_digest();assert(canonical_digest!=initial_digest);
    presentation=987654321;assert(vb_state_digest()==canonical_digest);
    const auto complete=capture();const auto canonical=capture(0);
    const auto offset=canonical.size()-sizeof(Session);
    assert(!std::memcmp(complete.data()+offset,&session,sizeof(session)));
    session.values[2]=999;presentation=0;vb_write8(0x05000000,42);
    assert(vb_state_restore(complete.data(),complete.size()));
    assert(loaded==1 && session.actors==4 && session.tick==17 && session.values[2]==30
           && presentation==987654321 && vb_read8(0x05000000)==0);
    auto invalid=complete;put32(invalid.data()+offset,5);crc(invalid);
    const auto before=capture();
    const auto before_generation=vb_state_generation();
    assert(vb_state_validate(complete.data(),complete.size(),[](void*)->int{return session.tick==17;},nullptr));
    assert(!vb_state_validate(complete.data(),complete.size(),[](void*)->int{return session.tick!=17;},nullptr));
    assert(!vb_state_validate(invalid.data(),invalid.size(),nullptr,nullptr));
    assert(capture()==before && loaded==1 && vb_state_generation()==before_generation);
    assert(!vb_state_restore(invalid.data(),invalid.size()));
    assert(capture()==before && loaded==1);
    // Invalid core state after a valid extension must undo both together.
    invalid=complete;invalid[112]=1;put32(invalid.data()+offset+4,100);crc(invalid);
    assert(!vb_state_validate(invalid.data(),invalid.size(),nullptr,nullptr));
    assert(capture()==before && loaded==1);
    assert(!vb_state_restore(invalid.data(),invalid.size()));
    assert(capture()==before && loaded==1);
    invalid=complete;put32(invalid.data()+112+offsetof(CPUState,sysreg)+24*sizeof(uint32_t),4);crc(invalid);
    assert(!vb_state_restore(invalid.data(),invalid.size()));
    assert(capture()==before && loaded==1);
    assert(vb_rewind_enable(4));vb_state_ui_init(argv[1]);assert(vb_rewind_record(10));
    vb_state_ui_record(10);assert(vb_state_ui_history_count()==1);
    const auto revision=vb_rewind_revision();
    for(unsigned i=0;i<1000;++i)vb_state_ui_sync();
    assert(vb_rewind_revision()==revision && vb_state_ui_history_count()==1);
    session.tick=20;session.values[0]=77;assert(vb_rewind_record(20));
    assert(vb_rewind_revision()!=revision);vb_state_ui_record(20);
    assert(vb_state_ui_history_count()==2);
    assert(vb_rewind_to(10));assert(session.tick==17 && session.values[0]==10 && loaded==2);
    assert(!vb_rewind_has(20));
    vb_state_ui_sync();assert(vb_state_ui_history_count()==1);
    vb_cart_ram_data()[123]=99;assert(vb_state_reset());
    assert(session.actors==0 && session.tick==0 && !session.values[0] && presentation==123);
    assert(vb_cart_ram_data()[123]==99 && loaded==3);
    vb_state_ui_sync();assert(!vb_state_ui_history_count());
    assert(vb_state_restore(reset.data(),reset.size()));assert(loaded==4);
    auto* network=vb_rollback_history_create(4);assert(network);
    assert(!vb_rollback_history_oldest(network,&pc));
    session.tick=30;assert(vb_rollback_history_save(network,100));
    session.tick=40;assert(vb_rollback_history_save(network,110));
    uint32_t oldest=0;assert(vb_rollback_history_oldest(network,&oldest) && oldest==100);
    assert(vb_rewind_record(50));
    assert(vb_rollback_history_load(network,100) && session.tick==30 && vb_rewind_has(50));
    assert(vb_rollback_history_has(network,110));
    vb_rollback_history_drop_after(network,100);assert(!vb_rollback_history_has(network,110));
    assert(vb_rewind_to(50) && session.tick==40 && vb_rollback_history_has(network,100));
    vb_rollback_history_destroy(network);
    vb_cart_ram_data()[999]=77;
    assert(!vb_state_previous_sram());assert(vb_state_restore(reset.data(),reset.size()));
    assert(old_sram_value==77 && !vb_state_previous_sram() && vb_cart_ram_data()[999]==0);
    vb_state_ui_shutdown();vb_state_ui_init(argv[1]);assert(vb_rewind_enable(2));
    for(unsigned tick:{100u,110u,120u}) {assert(vb_rewind_record(tick));vb_state_ui_record(tick);}
    vb_state_ui_sync();assert(vb_state_ui_history_count()==2 && vb_state_ui_selected_frame()==110);
    vb_rewind_shutdown();vb_state_ui_sync();assert(!vb_state_ui_history_count());
    vb_state_ui_shutdown();vb_state_shutdown();vb_memory_shutdown();
}
