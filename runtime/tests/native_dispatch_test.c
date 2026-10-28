#include "v810_interpreter.h"
#include <assert.h>
#include <string.h>

VbExecutionStats vb_execution;
static unsigned redirect_count, action;
static void fixture_execute(CPUState* cpu, unsigned index);
#include "dispatch_fixture.c"

static uint32_t rng=0x810;
static uint32_t random32(void) { rng^=rng<<13; rng^=rng>>17; rng^=rng<<5; return rng; }
static VbNativeFn reference_lookup(uint32_t pc) {
    unsigned lo=0,hi=sizeof(vb_routes)/sizeof(vb_routes[0]);
    while(lo<hi){unsigned mid=lo+(hi-lo)/2;if(vb_routes[mid].pc<pc)lo=mid+1;else hi=mid;}
    return lo<sizeof(vb_routes)/sizeof(vb_routes[0]) && vb_routes[lo].pc==pc ? vb_routes[lo].fn:0;
}
static void fixture_execute(CPUState* cpu, unsigned index) {
    if(!cpu->step_budget || cpu->cycles>=cpu->cycle_deadline){cpu->yielded=1;return;}
    --cpu->step_budget; cpu->cycles+=1+index%5;
    ++vb_execution.native_instructions;
    cpu->gpr[1+index%30]^=cpu->pc+index;
    if(action==1)cpu->halted=1;
    else if(action==2)cpu->yielded=1;
    if(redirect_count){--redirect_count;cpu->pc=vb_routes[(index+1)%(sizeof(vb_routes)/sizeof(vb_routes[0]))].pc;}
    else cpu->pc=cpu->gpr[31];
}
void vb_interpreter_step(CPUState* cpu) {
    ++cpu->cycles; ++vb_execution.interpreted;cpu->pc=cpu->gpr[31];
}
void vb_interpreter_fallback(CPUState* cpu) {
    ++vb_execution.fallback;cpu->pc=cpu->gpr[31];
}
int main(void) {
    const unsigned count=sizeof(vb_routes)/sizeof(vb_routes[0]);
    for(unsigned i=0;i<count;++i){
        uint32_t pc=vb_routes[i].pc;
        assert(vb_native_lookup(pc)==reference_lookup(pc));
        assert(vb_native_lookup(pc+1)==reference_lookup(pc+1));
        assert(vb_native_lookup(pc^0x80000000u)==reference_lookup(pc^0x80000000u));
    }
    for(unsigned i=0;i<1000000;++i){uint32_t pc=random32();assert(vb_native_lookup(pc)==reference_lookup(pc));}
    /* Compare ordinary routing with known-entry routing, including every gate,
       exact-address aliases, multi-entry transfers and unknown fallback. */
    for(unsigned i=0;i<40000;++i){
        unsigned index=random32()%count;uint32_t pc=vb_routes[index].pc;
        CPUState a={0},b; VbExecutionStats initial={0},sa,sb;
        a.cycles=random32()%10;a.cycle_deadline=i%7;a.step_budget=i%9;
        a.yielded=i%11==0;a.halted=i%13==0;
        uint32_t lp=i%17==0 ? pc : 0x12345679u;
        b=a;initial.mode=i%3;action=i%3;
        vb_execution=initial;redirect_count=i%5;vb_dispatch_call(&a,pc,lp);sa=vb_execution;
        vb_execution=initial;redirect_count=i%5;vb_dispatch_call_native(&b,pc,lp,vb_routes[index].fn);sb=vb_execution;
        assert(!memcmp(&a,&b,sizeof(a)));assert(!memcmp(&sa,&sb,sizeof(sa)));
        a=(CPUState){0};a.step_budget=1;a.cycle_deadline=10;b=a;
        vb_execution=initial;vb_dispatch_call(&a,pc|1u,lp);sa=vb_execution;
        vb_execution=initial;vb_dispatch_call_native(&b,pc|1u,lp,0);sb=vb_execution;
        assert(!memcmp(&a,&b,sizeof(a)));assert(!memcmp(&sa,&sb,sizeof(sa)));
    }
    return 0;
}
