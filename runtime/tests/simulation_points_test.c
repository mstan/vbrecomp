#include "simulation_points.h"
#include "v810_interpreter.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>

static uint8_t rom[65536];
static uint8_t read8(uint32_t a){return rom[a&65535];}
static uint16_t read16(uint32_t a){return read8(a)|(uint16_t)read8(a+1)<<8;}
static uint32_t read32(uint32_t a){return read16(a)|(uint32_t)read16(a+2)<<16;}
static void put16(unsigned a,uint16_t n){rom[a]=n;rom[a+1]=(uint8_t)(n>>8);}
void vb_stub_abort(const char* reason,uint32_t pc,uint32_t addr){(void)reason;(void)pc;(void)addr;abort();}
void vb_stub_abort_simple(const char* reason,uint32_t pc){vb_stub_abort(reason,pc,0);}
void vb_fntrace_record(uint32_t pc,uint32_t lp){(void)pc;(void)lp;}
static int redirect(CPUState* cpu,void* context) {
    unsigned* calls=(unsigned*)context;++*calls;++cpu->gpr[7];cpu->pc+=2;return 1;
}
static int observe(CPUState* cpu,void* context) {
    unsigned* calls=(unsigned*)context;++*calls;++cpu->gpr[6];return 0;
}
int main(void) {
    CPUState cpu;unsigned first=0,second=0;memset(&cpu,0,sizeof(cpu));
    cpu.pc=0xfff00100;cpu.read8=read8;cpu.read16=read16;cpu.read32=read32;
    put16(0x100,0x4141);put16(0x102,0x4142);put16(0x300,0x4143);
    assert(!vb_simulation_points_enable(1));
    assert(!vb_simulation_point_register(0xfff00101,0x4141,redirect,&first));
    assert(!vb_simulation_point_register(0x05000100,0x4141,redirect,&first));
    assert(vb_simulation_point_register(cpu.pc,0x4141,redirect,&first));
    assert(!vb_simulation_point_register(cpu.pc,0x4141,redirect,&first));
    // Disabled variants execute the original code, without calling adapters.
    vb_interpreter_step(&cpu);assert(cpu.gpr[10]==1 && !first && cpu.cycles==1);
    assert(vb_simulation_point_register(0xfff00300,0x4143,observe,&second));
    assert(vb_simulation_points_enable(1));
    assert(!vb_simulation_point_register(0xfff00400,0,redirect,&first));
    cpu.pc=0xfff00100;cpu.gpr[10]=0;cpu.cycles=0;
    vb_execution.breakpoint_pc=cpu.pc;vb_execution.breakpoint_enabled=1;
    vb_interpreter_step(&cpu);assert(!first && !cpu.cycles && cpu.pc==0xfff00100);
    vb_execution.breakpoint_enabled=0;vb_execution.stopped=0;
    vb_interpreter_step(&cpu);
    assert(first==1 && !second && cpu.gpr[7]==1 && cpu.gpr[10]==0 && !cpu.cycles && cpu.pc==0xfff00102 && cpu.yielded);
    vb_interpreter_step(&cpu);assert(cpu.gpr[10]==2 && cpu.cycles==1 && first==1);
    cpu.pc=0xfff00300;vb_interpreter_step(&cpu);
    assert(second==1 && cpu.gpr[6]==1 && cpu.gpr[10]==3 && first==1);
    vb_simulation_points_clear();cpu.pc=0xfff00100;vb_interpreter_step(&cpu);
    assert(first==1 && cpu.gpr[10]==1 && !vb_simulation_points_enabled);
}
