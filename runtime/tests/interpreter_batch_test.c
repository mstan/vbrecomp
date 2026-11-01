/* Compare the new exact-span interpreter to the frozen retained step loop.
 * Live buses observe architectural state and can modify upcoming code/mode.
 * Shared exception/extended helpers are unchanged by this optimization. */
#include "v810_interpreter.h"
#include "simulation_points.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern VbExecutionStats vb_reference_execution;
void vb_reference_interpreter_step(CPUState*);
void vb_reference_interpreter_fallback(CPUState*);
int vb_reference_fallback_coverage_get(unsigned,uint32_t*,uint64_t*);
uint64_t vb_reference_fallback_coverage_overflow(void);
static unsigned char memory[65536],initial_memory[65536],reference_memory[65536];
static CPUState* active;
static VbExecutionStats* execution;
static uint64_t trace;
static unsigned accesses,hook_calls,simulation_calls,change_mode,patch_code,scenario,next_mode;
static uint32_t random_state=0x81031;
static uint32_t random32(void) {random_state^=random_state<<13;random_state^=random_state>>17;random_state^=random_state<<5;return random_state;}
static void mix(uint64_t value) {trace=(trace^value)*UINT64_C(1099511628211);}
static void event(unsigned kind,uint32_t address,uint32_t value) {
    mix(kind);mix(address);mix(value);mix(active->pc);mix(active->cycles);
    mix(active->step_budget);mix(active->cycle_deadline);mix(vb_psw_pack(active));
    mix((unsigned char)active->pipeline_class);mix(active->bus_tail_cycles);
    for(unsigned i=0;i<32;++i)mix(active->gpr[i]);
    ++accesses;
    if(change_mode && accesses==change_mode)execution->mode=next_mode;
    if(patch_code && accesses==patch_code) {
        /* A fetch-side observer patches a later instruction; no stale decode. */
        memory[0x108]=0x47;memory[0x109]=0x41;
    }
}
static uint8_t read8(uint32_t a) {uint8_t v=memory[a&65535];event(1,a,v);return v;}
static uint16_t read16(uint32_t a) {uint16_t v=memory[a&65535]|(uint16_t)memory[(a+1)&65535]<<8;event(2,a,v);return v;}
static uint32_t read32(uint32_t a) {uint32_t v=0;for(unsigned i=0;i<4;++i)v|=(uint32_t)memory[(a+i)&65535]<<(i*8);event(3,a,v);return v;}
static void write8(uint32_t a,uint8_t v) {event(4,a,v);memory[a&65535]=v;}
static void write16(uint32_t a,uint16_t v) {event(5,a,v);memory[a&65535]=v;memory[(a+1)&65535]=(uint8_t)(v>>8);}
static void write32(uint32_t a,uint32_t v) {event(6,a,v);for(unsigned i=0;i<4;++i)memory[(a+i)&65535]=(uint8_t)(v>>(i*8));}
void vb_cpuhook_record(const CPUState* cpu) {assert(cpu==active);++hook_calls;event(7,cpu->pc,0);}
void vb_stub_abort(const char* why,uint32_t pc,uint32_t address) {fprintf(stderr,"unexpected abort: %s %08x %08x\n",why,pc,address);abort();}
void vb_stub_abort_simple(const char* why,uint32_t pc) {vb_stub_abort(why,pc,0);}
void vb_fntrace_record(uint32_t pc,uint32_t lp) {(void)pc;(void)lp;}
static int simulation(CPUState* cpu,void* unused) {
    (void)unused;++simulation_calls;++cpu->gpr[7];
    if(scenario==13) {cpu->pc+=2;return 1;}
    if(scenario==14)execution->mode=next_mode;
    return 0;
}
static void reference_run(CPUState* cpu,uint64_t lp) {
    while(cpu->pc!=lp && !cpu->halted && !cpu->yielded && vb_reference_execution.mode==VB_EXEC_INTERPRETER) {
        if(!cpu->step_budget || cpu->cycles>=cpu->cycle_deadline) {cpu->yielded=1;return;}
        --cpu->step_budget;vb_reference_interpreter_step(cpu);
    }
}
static void reference_generic(CPUState* cpu,uint64_t lp) {
    if(lp!=UINT64_MAX)cpu->gpr[31]=(uint32_t)lp;
    while(cpu->pc!=lp && !cpu->halted && !cpu->yielded) {
        if(!cpu->step_budget || cpu->cycles>=cpu->cycle_deadline) {cpu->yielded=1;return;}
        if(vb_reference_execution.mode==VB_EXEC_INTERPRETER) {
            --cpu->step_budget;vb_reference_interpreter_step(cpu);
        } else vb_reference_interpreter_fallback(cpu);
    }
}
static void candidate_forced(CPUState* cpu,uint64_t lp) {
#if VB_INTERPRETER_BATCH
    vb_interpreter_run(cpu,lp);
#else
    while(cpu->pc!=lp && !cpu->halted && !cpu->yielded && vb_execution.mode==VB_EXEC_INTERPRETER) {
        if(!cpu->step_budget || cpu->cycles>=cpu->cycle_deadline) {cpu->yielded=1;return;}
        --cpu->step_budget;vb_interpreter_step(cpu);
    }
#endif
}
static void put16(unsigned p,uint16_t word) {initial_memory[p]=word;initial_memory[p+1]=(uint8_t)(word>>8);}
static void setup_observers(CPUState* cpu,VbExecutionStats* stats) {
    active=cpu;execution=stats;trace=UINT64_C(1469598103934665603);
    accesses=hook_calls=simulation_calls=0;vb_simulation_points_clear();
    if(scenario>=12 && scenario<=14) {
        uint16_t word=initial_memory[0x100]|(uint16_t)initial_memory[0x101]<<8;
        assert(vb_simulation_point_register(cpu->pc,word,simulation,0));
        assert(vb_simulation_points_enable(1));
    }
}
#ifdef VB_TEST_COVERAGE_EDGES
void vb_test_coverage_clear(void);
void vb_test_coverage_seed(unsigned,uint64_t);
void vb_test_coverage_seed_overflow(uint64_t);
void vb_reference_test_coverage_clear(void);
void vb_reference_test_coverage_seed(unsigned,uint64_t);
void vb_reference_test_coverage_seed_overflow(uint64_t);
static void compare_coverage(void) {
    for(unsigned slot=0;slot<VB_FALLBACK_COVERAGE_CAPACITY;++slot) {
        uint32_t a=0,b=0;uint64_t ah=0,bh=0;
        assert(vb_fallback_coverage_get(slot,&a,&ah)==vb_reference_fallback_coverage_get(slot,&b,&bh));
        assert(a==b && ah==bh);
    }
    assert(vb_fallback_coverage_overflow()==vb_reference_fallback_coverage_overflow());
}
static void coverage_step(uint32_t pc) {
    CPUState initial={0};initial.pc=pc;initial.step_budget=1;initial.cycle_deadline=100;
    initial.read8=read8;initial.read16=read16;initial.read32=read32;
    initial.write8=write8;initial.write16=write16;initial.write32=write32;
    CPUState expected=initial,actual=initial;
    vb_reference_execution=(VbExecutionStats){0};vb_execution=(VbExecutionStats){0};
    memcpy(memory,initial_memory,sizeof(memory));setup_observers(&expected,&vb_reference_execution);
    vb_reference_interpreter_fallback(&expected);uint64_t expected_trace=trace;
    unsigned expected_accesses=accesses,expected_hooks=hook_calls;
    memcpy(reference_memory,memory,sizeof(memory));
    memcpy(memory,initial_memory,sizeof(memory));setup_observers(&actual,&vb_execution);
    vb_interpreter_fallback(&actual);
    assert(!memcmp(&actual,&expected,sizeof(actual)));
    assert(!memcmp(&vb_execution,&vb_reference_execution,sizeof(vb_execution)));
    assert(!memcmp(memory,reference_memory,sizeof(memory)) && trace==expected_trace);
    assert(accesses==expected_accesses && hook_calls==expected_hooks);
}
static void coverage_edges(void) {
    vb_test_coverage_clear();vb_reference_test_coverage_clear();
    memset(initial_memory,0,sizeof(initial_memory)); /* MOV r0,r0 at every fetch. */
    scenario=change_mode=patch_code=0;
    unsigned inverse=0;
    while(((inverse*2654435761u)&(VB_FALLBACK_COVERAGE_CAPACITY-1))!=1)++inverse;
    const uint32_t a=0x20000,c=a+0x2000,b=inverse*2;
    coverage_step(a);coverage_step(c);coverage_step(b); /* Slots0,1,2; distinct cache sets for c/b. */
    for(unsigned i=0,used=3;used<VB_FALLBACK_COVERAGE_CAPACITY;++i) {
        if(i*2==b)continue;
        coverage_step(i*2);++used;
    }
    compare_coverage();
    /* A wrapping row creates a hole before a cached positive in another set.
     * The original probe now inserts a duplicate PC into that earlier slot. */
    coverage_step(b);vb_test_coverage_seed(1,UINT64_MAX);vb_reference_test_coverage_seed(1,UINT64_MAX);
    coverage_step(c);coverage_step(b);compare_coverage();
    const uint32_t missing=0x100000+inverse*4; /* Hash2; absent from the full table. */
    for(unsigned i=0;i<65;++i)coverage_step(missing);
    assert(vb_fallback_coverage_overflow()==65);compare_coverage();
    vb_test_coverage_seed_overflow(UINT64_MAX);vb_reference_test_coverage_seed_overflow(UINT64_MAX);
    coverage_step(missing);coverage_step(missing);
    assert(vb_fallback_coverage_overflow()==1);compare_coverage();
    /* A previously cached negative must become an insertion after another
     * counter wraps; neither its old miss nor cached positives may survive. */
    vb_test_coverage_seed(0,UINT64_MAX);vb_reference_test_coverage_seed(0,UINT64_MAX);
    coverage_step(a);coverage_step(missing);
    assert(vb_fallback_coverage_overflow()==1);compare_coverage();
    for(unsigned i=0;i<256;++i)coverage_step(0x400000+2*(i*256+5));
    for(unsigned i=0;i<32;++i)coverage_step(0); /* Zero is a real PC, not a cache sentinel. */
    compare_coverage();
    puts("PASS: original full coverage slots/hits, repeated misses, cache aliases, PC0 and independent hit/overflow counter wraps");
}
#endif
int main(void) {
    unsigned total_steps=0,long_steps=0,observed_hooks=0,observed_callbacks=0;
    const unsigned ops[]={0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,23,24,25,26,29,30,31,32,34,36,38,40,41,42,43,44,45,46,47,48,49,51,52,53,55,56,57,58,59,60,61,62,63};
    /* Direct forced span and the real no_game_linked dispatch in forced and
     * default zero-valued HYBRID modes. OFF uses the original public step. */
    for(unsigned profile=0;profile<3;++profile)for(unsigned test=0;test<12000;++test) {
        scenario=test%16;
        for(unsigned i=0;i<65536;i+=2)put16(i,0x6800); /* HALT outside the program. */
        unsigned pc=0x100;
        for(unsigned i=0;i<48;++i) {
            unsigned op=ops[random32()%(sizeof(ops)/sizeof(*ops))],r1=random32()%32,r2=random32()%32;
            if(op==31)r1=8+random32()%8; /* bounded bit-string word operation */
            put16(pc,(uint16_t)((op<<10)|(r2<<5)|r1));pc+=2;
            if(op>=40) {put16(pc,(uint16_t)random32());pc+=2;}
        }
        if(scenario>=12 && scenario<=14)put16(0x100,0x4141);
        if(scenario==15) {
            /* A long arithmetic/load/store loop exercises repeated exact exits,
             * rather than letting every randomized program quickly HALT. */
            put16(0x100,(1<<10)|(10<<5)|1);
            put16(0x102,(14<<10)|(11<<5)|10);
            put16(0x104,(55<<10)|(10<<5)|2);put16(0x106,0);
            put16(0x108,(51<<10)|(12<<5)|2);put16(0x10a,0);
            put16(0x10c,0x8000|(5<<9)|((-12)&0x1ff));
        }
        CPUState initial={0};
        for(unsigned i=0;i<32;++i)initial.gpr[i]=random32();
        initial.pc=0xfff00100;initial.gpr[28]=random32()%65;
        initial.gpr[29]=0x1000;initial.gpr[30]=0x1200;initial.gpr[31]=0x800;
        if(scenario==15) {initial.gpr[1]=1;initial.gpr[2]=0x1000;}
        initial.sysreg[VB_SR_EIPC]=initial.sysreg[VB_SR_FEPC]=0x800;
        initial.psw_z=random32()&1;initial.psw_s=random32()&1;initial.psw_ov=random32()&1;initial.psw_cy=random32()&1;
        initial.cycles=random32()%20;initial.cycle_deadline=initial.cycles+random32()%160;
        initial.step_budget=random32()%65;initial.pipeline_class=(int8_t)((int)(random32()%6)-1);
        initial.read8=read8;initial.read16=read16;initial.read32=read32;
        initial.write8=write8;initial.write16=write16;initial.write32=write32;
        if(scenario==1)initial.step_budget=0;
        if(scenario==2)initial.cycle_deadline=initial.cycles;
        if(scenario==3)initial.halted=1;
        if(scenario==4)initial.yielded=1;
        uint64_t lp=scenario==5?initial.pc:scenario==6?initial.pc+4:UINT64_MAX;
        VbExecutionStats stats={0};
        if(profile!=2)stats.mode=VB_EXEC_INTERPRETER;
        next_mode=profile==2?VB_EXEC_INTERPRETER:VB_EXEC_HYBRID;
        if(scenario==7 || scenario==8) {stats.breakpoint_enabled=1;stats.breakpoint_pc=initial.pc+(scenario==8?2:0);}
        if(scenario==9) {stats.stepping=1;stats.step_remaining=random32()%4;}
        change_mode=scenario==10?3:0;patch_code=scenario==11?2:0;
        CPUState expected=initial,actual=initial;
        memcpy(memory,initial_memory,sizeof(memory));vb_reference_execution=stats;
        setup_observers(&expected,&vb_reference_execution);
        if(profile)reference_generic(&expected,lp);else reference_run(&expected,lp);
        uint64_t expected_trace=trace;unsigned expected_accesses=accesses,expected_hooks=hook_calls,expected_callbacks=simulation_calls;
        memcpy(reference_memory,memory,sizeof(memory));
        memcpy(memory,initial_memory,sizeof(memory));vb_execution=stats;
        setup_observers(&actual,&vb_execution);
        if(!profile)candidate_forced(&actual,lp);
        else if(lp==UINT64_MAX)vb_dispatch(&actual,actual.pc);
        else vb_dispatch_call(&actual,actual.pc,(uint32_t)lp);
        if(memcmp(&actual,&expected,sizeof(actual)) || memcmp(&vb_execution,&vb_reference_execution,sizeof(stats)) || memcmp(memory,reference_memory,sizeof(memory)) || trace!=expected_trace || accesses!=expected_accesses || hook_calls!=expected_hooks || simulation_calls!=expected_callbacks) {
            fprintf(stderr,"differential mismatch profile%u case%u scenario%u PC%08x/%08x cycles%llu/%llu\n",profile,test,scenario,actual.pc,expected.pc,(unsigned long long)actual.cycles,(unsigned long long)expected.cycles);return 1;
        }
        total_steps+=(unsigned)vb_execution.interpreted;observed_hooks+=hook_calls;observed_callbacks+=simulation_calls;
        if(scenario==15)long_steps+=(unsigned)vb_execution.interpreted;
    }
    for(unsigned slot=0;slot<VB_FALLBACK_COVERAGE_CAPACITY;++slot) {
        uint32_t pc=0,expected_pc=0;uint64_t hits=0,expected_hits=0;
        assert(vb_fallback_coverage_get(slot,&pc,&hits)==vb_reference_fallback_coverage_get(slot,&expected_pc,&expected_hits));
        assert(pc==expected_pc && hits==expected_hits);
    }
    assert(vb_fallback_coverage_overflow()==vb_reference_fallback_coverage_overflow());
#ifdef VB_TEST_COVERAGE_EDGES
    coverage_edges();
#endif
    assert(total_steps && observed_callbacks && long_steps>1000);
#ifdef VB_CPUHOOK_ENABLE
    assert(observed_hooks);
#endif
    printf("PASS:36000 frozen-original differential cases across forced span, generic forced and default HYBRID; %u steps, %u hooks, %u simulation callbacks; exact fallback coverage, live bus/state traces, code patches, mutable code, deadlines/overshoot, budgets, caller returns, debug stops, halt/yield, exceptions and bidirectional mode changes\n",total_steps,observed_hooks,observed_callbacks);
    printf("PASS: %u instructions in bounded arithmetic/load/store loop spans\n",long_steps);
    return 0;
}
