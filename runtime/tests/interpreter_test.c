#include "v810_interpreter.h"
#include "interrupts.h"
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static unsigned char ram[65536];
static uint8_t read8(uint32_t a) { return ram[a&65535]; }
static uint16_t read16(uint32_t a) { return read8(a)|((uint16_t)read8(a+1)<<8); }
static uint32_t read32(uint32_t a) { return read16(a)|((uint32_t)read16(a+2)<<16); }
static void write8(uint32_t a,uint8_t v) { ram[a&65535]=v; }
static void write16(uint32_t a,uint16_t v) { write8(a,(uint8_t)v);write8(a+1,(uint8_t)(v>>8)); }
static void write32(uint32_t a,uint32_t v) { write16(a,(uint16_t)v);write16(a+2,(uint16_t)(v>>16)); }
void vb_stub_abort(const char* why,uint32_t pc,uint32_t addr) { (void)why;(void)pc;(void)addr;abort(); }
void vb_stub_abort_simple(const char* why,uint32_t pc) { vb_stub_abort(why,pc,0); }
void vb_fntrace_record(uint32_t pc,uint32_t lp) { (void)pc;(void)lp; }

static CPUState setup(void) {
    CPUState c; memset(&c,0,sizeof(c)); memset(ram,0,sizeof(ram));
    c.pc=0x100; c.read8=read8;c.read16=read16;c.read32=read32;
    c.write8=write8;c.write16=write16;c.write32=write32;
    c.step_budget=100; c.cycle_deadline=UINT64_MAX;
    return c;
}
static void op(CPUState* c,unsigned opcode,unsigned r1,unsigned r2,uint16_t imm) {
    write16(c->pc,(uint16_t)((opcode<<10)|(r2<<5)|r1));
    write16(c->pc+2,imm); vb_interpreter_step(c);
}
int main(int argc,char** argv) {
    CPUState c=setup();
    /* RETI must yield before the restored PC executes if a lower-priority
     * source is already pending. This is the Zero Racers frame-212 case. */
    vb_irq_init();c.psw_ep=1;c.psw_id=1;c.psw_int_level=5;c.bstr_src_valid=1;
    c.sysreg[VB_SR_EIPC]=0x200;c.sysreg[VB_SR_EIPSW]=1;
    vb_irq_raise(VBIRQ_SOURCE_TIMER);
    assert(!vb_irq_check_and_deliver(&c));
    op(&c,0x19,0,0,0);
    assert(c.pc==0x200 && c.cycle_deadline==c.cycles);
    assert(vb_irq_check_and_deliver(&c));
    assert(c.pc==0xfffffe10 && c.sysreg[VB_SR_EIPC]==0x200 && c.sysreg[VB_SR_EIPSW]==1);
    assert(!c.bstr_src_valid);
    for (int unmask=0;unmask<2;++unmask) {
        c=setup();c.psw_id=1;c.gpr[2]=0;
        op(&c,unmask ? 0x1c:0x16,unmask ? 5:0,2,0);
        assert(c.pc==0x102 && c.cycle_deadline==c.cycles);
        assert(vb_irq_check_and_deliver(&c) && c.sysreg[VB_SR_EIPC]==0x102);
    }
    vb_irq_init();c=setup();
    /* Pairing survives intervening native/interpreted dispatch boundaries. */
    c.gpr[1]=0x800;
    op(&c,0x33,1,2,0);assert(c.cycles==5);
    op(&c,0x31,1,2,0);assert(c.cycles==7);
    op(&c,8,0,2,0);assert(c.cycles==20);
    op(&c,0x33,1,2,0);assert(c.cycles==21);
    op(&c,0x37,1,2,0);assert(c.cycles==22);
    op(&c,0x37,1,2,0);assert(c.cycles==26);
    op(&c,0x3b,1,2,0);assert(c.cycles==31);
    c=setup();
    c.gpr[1]=1;c.gpr[2]=0x7fffffff;op(&c,1,1,2,0);
    assert(c.gpr[2]==0x80000000 && c.psw_ov && c.psw_s && !c.psw_cy);
    c.gpr[2]=0xffffffff;op(&c,1,1,2,0);assert(!c.gpr[2] && c.psw_z && c.psw_cy && !c.psw_ov);
    c.gpr[2]=0;op(&c,2,1,2,0);assert(c.gpr[2]==0xffffffff && c.psw_s && c.psw_cy);
    c.gpr[1]=0x80000000;op(&c,0x29,1,0,0x8000);assert(c.gpr[0]==0 && c.psw_ov);
    c.gpr[2]=0x80000001;op(&c,0x14,1,2,0);assert(c.gpr[2]==2 && c.psw_cy);
    op(&c,0x15,0,2,0);assert(c.gpr[2]==2 && !c.psw_cy);
    c.gpr[2]=0x80000000;op(&c,0x17,31,2,0);assert(c.gpr[2]==0xffffffff && !c.psw_cy);
    c.gpr[1]=2;c.gpr[30]=0x7fffffff;op(&c,8,1,30,0);assert(c.gpr[30]==0xfffffffe && c.psw_ov);
    c.gpr[1]=0xffffffff;c.gpr[2]=0x80000000;op(&c,9,1,2,0);assert(c.gpr[2]==0x80000000 && c.gpr[30]==0 && c.psw_ov);
    c.gpr[1]=3;c.gpr[30]=10;op(&c,11,1,30,0);assert(c.gpr[30]==3);
    c.gpr[1]=0;uint32_t div_pc=c.pc;op(&c,9,1,2,0);assert(c.pc==VB_ZERO_DIV_HANDLER && c.sysreg[VB_SR_EIPC]==div_pc);
    c=setup();c.gpr[1]=0x803;write32(0x800,0x80ff80ff);
    op(&c,0x31,1,2,0);assert(c.gpr[2]==0xffff80ff);
    op(&c,0x39,1,2,0);assert(c.gpr[2]==0x80ff);
    op(&c,0x38,1,2,0);assert(c.gpr[2]==0x80);
    op(&c,0x33,1,2,0);assert(c.gpr[2]==0x80ff80ff);
    c.gpr[2]=0x80ff80ff;c.gpr[30]=0xaabbccdd;op(&c,0x3a,1,2,0);
    assert(c.gpr[2]==0x80ff80ff && read32(0x800)==0xaabbccdd && c.psw_z);
    c.gpr[2]=0;op(&c,0x3a,1,2,0);assert(c.gpr[2]==0xaabbccdd && read32(0x800)==0xaabbccdd && !c.psw_z);
    c=setup();op(&c,0x18,5,0,0);assert(c.sysreg[VB_SR_EIPC]==0x102 && c.pc==0xffffffa0);
    op(&c,0x19,0,0,0);assert(c.pc==0x102 && !c.psw_ep);
    op(&c,0x1a,0,0,0);assert(c.halted && c.pc==0x104);
    c=setup();c.gpr[2]=0x12345678;op(&c,0x3e,0,2,8<<10);assert(c.gpr[2]==0x12347856);
    c.gpr[1]=1;op(&c,0x3e,1,2,10<<10);assert(c.gpr[2]==0x80000000);
    c.gpr[1]=0x3f800000;c.gpr[2]=0x40000000;op(&c,0x3e,1,2,4<<10);assert(c.gpr[2]==0x40400000 && !c.psw_z);
    c.gpr[1]=0x40200000;op(&c,0x3e,1,2,3<<10);assert(c.gpr[2]==2 && c.psw_fpr);
    c=setup();c.gpr[1]=1;op(&c,0x3e,1,2,4<<10);assert(c.psw_fro && c.pc==VB_FPU_HANDLER);
    c=setup();c.gpr[1]=0;c.gpr[2]=0x3f800000;op(&c,0x3e,1,2,7<<10);assert(c.psw_fzd && c.gpr[2]==0x3f800000);
    c=setup();c.gpr[26]=30;c.gpr[27]=29;c.gpr[28]=6;c.gpr[29]=0x900;c.gpr[30]=0x800;
    write32(0x800,0xa0000000);write32(0x804,3);write32(0x900,0xffffffff);
    op(&c,0x1f,11,0,0);assert(c.pc==0x100 && c.gpr[28]==4);
    vb_interpreter_step(&c);assert(c.pc==0x102 && !c.gpr[28]);
    assert(read32(0x900)==0x7fffffff && read32(0x904)==7);
    c=setup();c.gpr[27]=0;c.gpr[28]=32;c.gpr[30]=0x800;write32(0x800,0x10);
    op(&c,0x1f,2,0,0);assert(!c.psw_z && c.gpr[29]==4 && c.gpr[27]==3 && c.gpr[28]==28);
    puts("V810 directed semantics passed");
    if (argc>1) {
        for(int mode=0;mode<=2;mode+=2) {
            c=setup();FILE* f=fopen(argv[1],"rb");assert(f);assert(fread(ram,1,sizeof(ram),f)==sizeof(ram));fclose(f);
            c.pc=0x07000020;c.gpr[4]=0x05008000;c.gpr[10]=7;
            write16(0x8000,(0x11<<10)|(10<<5)|3);write16(0x8002,(6<<10)|31);
            memset(&vb_execution,0,sizeof(vb_execution));vb_execution.mode=mode;
            for(int n=0;n<20 && !c.halted;++n) {
                c.step_budget=1;c.yielded=0;
                vb_dispatch(&c,c.pc);
            }
            assert(c.halted && c.gpr[10]==12 && c.pc==0x07000028 && c.gpr[31]==0x07000024);
            if(mode==0) assert(vb_execution.fallback && vb_execution.native_entries);
            /* No guest address doubles as a host return sentinel. */
            c=setup();c.pc=0xdead0000;write16(0,(0x1a<<10));
            vb_dispatch(&c,c.pc);
            assert(c.halted && c.pc==0xdead0002);
        }
        puts("Native/RAM-interpreter return and instruction-yield bridge passed");
        for(int enabled=0;enabled<2;++enabled) for(int mode=0;mode<=2;mode+=2) {
            c=setup();FILE* f=fopen(argv[1],"rb");assert(f);assert(fread(ram,1,sizeof(ram),f)==sizeof(ram));fclose(f);
            vb_branch_rules_reset();
            if(enabled) assert(vb_branch_rule_add(0x0700010a,0x8c06,11,VB_BRANCH_LT,800));
            c.pc=0x07000100;memset(&vb_execution,0,sizeof(vb_execution));vb_execution.mode=mode;
            vb_dispatch(&c,c.pc);
            unsigned expected=0;
#ifdef VB_BRANCH_OVERRIDES
            expected=(unsigned)enabled;
#endif
            assert(c.halted && c.gpr[10]==expected && c.gpr[11]==500);
            assert(!c.psw_s && !c.psw_ov); /* Comparisons do not change PSW. */
            if(mode==0) assert(vb_execution.native_instructions && !vb_execution.fallback);
        }
        vb_branch_rules_reset();
        assert(vb_branch_rule_add(0x0700010a,0x8c06,11,VB_BRANCH_LT,800));
        assert(!vb_branch_rule_add(0x0700010a,0x8c06,11,VB_BRANCH_LT,801));
        vb_branch_rules_enable(0);assert(!VB_BRANCH_PREDICATE(&c,0x0700010a,0x8c06,0));
        vb_branch_rules_reset();
        assert(!vb_branch_rule_add(0x0700010b,0x8c06,11,VB_BRANCH_LT,800));
        assert(!vb_branch_rule_add(0x0500010a,0x8c06,11,VB_BRANCH_LT,800));
        assert(!vb_branch_rule_add(0x0700010a,0x8c06,0,VB_BRANCH_LT,800));
        assert(!vb_branch_rule_add(0x0700010a,0x8c06,11,99,800));
        assert(!vb_branch_rule_add(0x0700010a,0x9a00,11,VB_BRANCH_LT,800));
        for(unsigned n=0;n<VB_BRANCH_RULE_CAPACITY;++n) assert(vb_branch_rule_add(0x07000000+n*2,0x8c06,11,VB_BRANCH_LT,800));
        assert(!vb_branch_rule_add(0x07001000,0x8c06,11,VB_BRANCH_LT,800));
        c.gpr[11]=500;
        assert(vb_branch_predicate_lookup(&c,0xff000000,0x8c06,0)==1);
        assert(vb_branch_predicate_lookup(&c,0x07000000,0x8c08,0)==0);
        assert(vb_branch_predicate_lookup(&c,0x07001000,0x8c06,0)==0);
        unsigned observed=0;uint64_t hits=0;
        for(unsigned n=0;n<VB_BRANCH_RULE_SLOTS;++n) { VbBranchRule row;if(vb_branch_rule_get(n,&row)) { ++observed;hits+=row.hits; } }
        assert(observed==VB_BRANCH_RULE_CAPACITY && hits==1);
        vb_branch_rules_reset();assert(!vb_branch_rule_count);
        for(unsigned comparison=VB_BRANCH_LT;comparison<=VB_BRANCH_GE;++comparison) {
            vb_branch_rules_reset();assert(vb_branch_rule_add(0x0700010a,0x8c06,11,comparison,0));
            for(int value=-100;value<=100;value+=100) {
                c.gpr[11]=(uint32_t)value;
                int expected=comparison==VB_BRANCH_LT ? value<0 : comparison==VB_BRANCH_LE ? value<=0 : comparison==VB_BRANCH_GT ? value>0 : value>=0;
                assert(vb_branch_predicate_lookup(&c,0x0700010a,0x8c06,!expected)==expected);
            }
        }
        vb_branch_rules_reset();
        puts("Guarded branch rules: native/interpreter, stock/reset, aliases, bounds and capacity passed");
    }
    c=setup();memset(&vb_execution,0,sizeof(vb_execution));
    vb_execution.breakpoint_enabled=1;vb_execution.breakpoint_pc=c.pc;
    vb_interpreter_fallback(&c);
    assert(c.pc==0x100 && c.step_budget==100 && !vb_execution.fallback && !vb_execution.interpreted);
    assert(vb_execution.stopped);
    /* Fill the bounded fallback map, then verify collision accounting and
     * explicit overflow without dropping hits or writing past the table. */
    uint64_t previous_hits=vb_fallback_coverage_overflow();
    for(unsigned n=0;n<VB_FALLBACK_COVERAGE_CAPACITY;++n) {
        uint32_t pc;uint64_t hits;
        if(vb_fallback_coverage_get(n,&pc,&hits)) previous_hits+=hits;
    }
    c=setup();memset(&vb_execution,0,sizeof(vb_execution));
    for (unsigned n=0;n<VB_FALLBACK_COVERAGE_CAPACITY+8;++n) {
        c.pc=0x10000u+n*2;c.step_budget=1;
        vb_interpreter_fallback(&c);
    }
    uint64_t recorded=0;unsigned occupied=0;
    for(unsigned n=0;n<VB_FALLBACK_COVERAGE_CAPACITY;++n) {
        uint32_t pc;uint64_t hits;
        if(vb_fallback_coverage_get(n,&pc,&hits)) { assert(!(pc&1));recorded+=hits;++occupied; }
    }
    assert(occupied==VB_FALLBACK_COVERAGE_CAPACITY);
    assert(vb_fallback_coverage_overflow()>=8);
    assert(recorded+vb_fallback_coverage_overflow()==previous_hits+vb_execution.fallback);
    uint32_t ignored_pc;uint64_t ignored_hits;
    assert(!vb_fallback_coverage_get(VB_FALLBACK_COVERAGE_CAPACITY,&ignored_pc,&ignored_hits));
    puts("Bounded fallback PC coverage and overflow passed");
    return 0;
}
