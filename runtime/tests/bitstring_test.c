#include "v810_bitstring.h"
#include "interrupts.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

void vb_bitstring_lle_reference(CPUState *, unsigned);
enum { MEMORY = 8192, EVENTS = 10000 };
typedef struct Event { uint32_t addr, value; int write; } Event;
static uint8_t memory[MEMORY], reference_memory[MEMORY];
static Event events[EVENTS], reference_events[EVENTS];
static unsigned count, rng = 0x4d7b1083;
static uint32_t random32(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5; return rng;
}
static uint32_t read32(uint32_t addr) {
    uint32_t value = 0;
    for (unsigned i=0; i<4; ++i) value |= (uint32_t)memory[(addr+i)%MEMORY] << (i*8);
    assert(count < EVENTS); events[count++] = (Event){addr,value,0}; return value;
}
static void write32(uint32_t addr, uint32_t value) {
    assert(count < EVENTS); events[count++] = (Event){addr,value,1};
    for (unsigned i=0; i<4; ++i) memory[(addr+i)%MEMORY] = value >> (i*8);
}
void vb_exception(CPUState *cpu, uint32_t handler, uint16_t ecode) {
    cpu->pc = handler; cpu->sysreg[VB_SR_ECR] = ecode;
}
static void compare(unsigned op, unsigned length, unsigned source_offset,
                    unsigned dest_offset, int overlap, int cached) {
    CPUState initial = {0};
    for (unsigned i=0;i<32;++i) initial.gpr[i]=random32();
    initial.pc=0x400; initial.cycles=19; initial.psw_z=1;
    initial.gpr[26]=dest_offset; initial.gpr[27]=source_offset;
    initial.gpr[28]=length; initial.gpr[30]=overlap==3 ? 0xfffffffcu : 0x200;
    initial.gpr[29]=initial.gpr[30]+(overlap==0 ? 0x500 : overlap==1 ? 0 : overlap==2 ? 4 : -4u);
    initial.bstr_src_valid=cached; initial.bstr_src_cache=random32();
    initial.read32=read32; initial.write32=write32;
    for(unsigned i=0;i<MEMORY;++i) memory[i]=random32();
    uint8_t original[MEMORY]; memcpy(original,memory,MEMORY);
    CPUState floor=initial; count=0; unsigned calls=0;
    do {
        /* First entry's caller charge is common. Logical redispatches each
         * charge one extra base cycle outside the original service. */
        if (calls && op>=8 && op<16) ++floor.cycles;
        vb_bitstring_lle_reference(&floor,op); assert(++calls<10000);
    }
    while(floor.pc==initial.pc);
    unsigned reference_count=count;
    memcpy(reference_memory,memory,MEMORY);
    memcpy(reference_events,events,count*sizeof(Event));
    CPUState native=initial; count=0; memcpy(memory,original,MEMORY);
    vb_bitstring_execute(&native,op);
    assert(!memcmp(&native,&floor,sizeof(native)));
    assert(!memcmp(memory,reference_memory,MEMORY));
    assert(count==reference_count);
    assert(!memcmp(events,reference_events,count*sizeof(Event)));
}
int main(void) {
    unsigned cases=0;
    const unsigned lengths[]={0,1,2,7,31,32,33,63,64,65,255,2048,65536};
    for(unsigned op=8;op<16;++op)
        for(unsigned s=0;s<32;++s)
            for(unsigned d=0;d<32;++d) {
                compare(op,lengths[(s+d)%12],s,d,(s+d)%4,(s^d)&1);
                ++cases;
            }
    for(unsigned op=8;op<16;++op) { compare(op,65536,0,0,0,0); ++cases; }
    /* Unsupported/search op handling stays on the original shared floor. */
    for(unsigned op=0;op<32;++op) {
        if(op>=8 && op<16) continue;
        compare(op,31,1,3,0,0); ++cases;
    }
    printf("bit-string completed-contract differential passed: %u cases\n",cases);
    return 0;
}
