/* V810 fetch/decode/execute fallback. The independent reference is Beetle VB,
 * hosted in another process. ISA references: NEC V810 Architecture Manual,
 * instruction descriptions; local Beetle v810_oploop.inc for NVC details. */
#include "v810_interpreter.h"
#include "interrupts.h"
#include "stub_abort.h"
#include <limits.h>

VbExecutionStats vb_execution;

int vb_execution_debug_check(CPUState* c) {
    int stop=vb_execution.stepping && !vb_execution.step_remaining;
    stop=stop || (vb_execution.breakpoint_enabled && c->pc==vb_execution.breakpoint_pc && !vb_execution.skip_break);
    vb_execution.skip_break=0;
    if(stop) { vb_execution.stopped=1;c->yielded=1;return 1; }
    if(vb_execution.stepping) --vb_execution.step_remaining;
    return 0;
}

static void sz(CPUState* c, uint32_t r) { c->psw_z = r == 0; c->psw_s = r >> 31; }
static uint32_t add(CPUState* c, uint32_t a, uint32_t b) {
    uint32_t r = a + b; sz(c,r); c->psw_cy = r < a;
    c->psw_ov = (~(a^b) & (a^r)) >> 31; return r;
}
static uint32_t sub(CPUState* c, uint32_t a, uint32_t b) {
    uint32_t r = a - b; sz(c,r); c->psw_cy = a < b;
    c->psw_ov = ((a^b) & (a^r)) >> 31; return r;
}
static int condition(const CPUState* c, unsigned n) {
    int yes;
    switch (n & 7) {
    case 0: yes=c->psw_ov; break;
    case 1: yes=c->psw_cy; break;
    case 2: yes=c->psw_z; break;
    case 3: yes=c->psw_cy || c->psw_z; break;
    case 4: yes=c->psw_s; break;
    case 5: yes=1; break;
    case 6: yes=c->psw_s != c->psw_ov; break;
    default: yes=(c->psw_s != c->psw_ov) || c->psw_z; break;
    }
    return (n & 8) ? !yes : yes;
}
static uint32_t shift(CPUState* c, uint32_t a, unsigned sh, unsigned op) {
    uint32_t r;
    sh &= 31;
    if (op == 4) {
        r = a << sh; c->psw_cy = sh ? (a >> (32-sh)) & 1 : 0;
    } else {
        r = op == 7 ? (uint32_t)((int32_t)a >> sh) : a >> sh;
        c->psw_cy = sh ? (a >> (sh-1)) & 1 : 0;
    }
    sz(c,r); c->psw_ov=0; return r;
}
void vb_interpreter_step(CPUState* c) {
    if (vb_execution_before(c)) return;
    static const uint8_t costs[64] = {
        1,1,1,1,1,1,3,1,13,38,13,36,1,1,1,1,
        1,1,1,1,1,1,1,1,15,10,1,1,1,1,1,1,
        1,1,1,1,1,1,1,1,1,1,3,3,1,1,1,1,
        1,1,1,1,1,1,1,1,3,3,26,3,1,1,1,1
    };
    uint32_t pc=c->pc;
    uint16_t word=c->read16(pc & ~1u);
    unsigned op=word >> 10, r1=word & 31, r2=(word >> 5)&31;
    uint32_t a=c->gpr[r2], b=c->gpr[r1], result=0;
    uint16_t imm=op>=0x28 ? c->read16((pc+2)&~1u) : 0;
    uint32_t next=pc+(op>=0x28 ? 4:2), addr=b+(uint32_t)(int32_t)(int16_t)imm;
    int write=0;
    c->gpr[0]=0;
    VB_CPUHOOK(c);
    ++vb_execution.interpreted;
    vb_execution_charge(c,op,costs[op]);
    if (op>=0x20 && op<=0x27) {
        if (condition(c,(word>>9)&15)) {
            next=pc+(uint32_t)(((int32_t)(word&0x1ff)^0x100)-0x100);
            c->cycles+=2;
        }
    } else switch(op) {
    case 0x00: result=b; write=1; break;
    case 0x01: result=add(c,a,b); write=1; break;
    case 0x02: result=sub(c,a,b); write=1; break;
    case 0x03: sub(c,a,b); break;
    case 0x04: case 0x05: case 0x07: result=shift(c,a,b,op); write=1; break;
    case 0x06: next=b&~1u; break;
    case 0x08: case 0x0a: {
        uint64_t product=op==8 ? (uint64_t)((int64_t)(int32_t)a*(int64_t)(int32_t)b) : (uint64_t)a*b;
        result=(uint32_t)product; c->gpr[30]=(uint32_t)(product>>32);
        c->psw_ov=op==8 ? product!=(uint64_t)(int64_t)(int32_t)result : (product>>32)!=0;
        sz(c,r2 ? result:0); write=1; break;
    }
    case 0x09: case 0x0b: {
        if (!b) { vb_exception(c,VB_ZERO_DIV_HANDLER,VB_ECODE_ZERO_DIV); return; }
        c->psw_ov=0;
        if (op==9) {
            if (a==0x80000000u && b==0xffffffffu) { result=a; c->gpr[30]=0; c->psw_ov=1; }
            else { result=(uint32_t)((int32_t)a/(int32_t)b); c->gpr[30]=(uint32_t)((int32_t)a%(int32_t)b); }
        } else { result=a/b; c->gpr[30]=a%b; }
        sz(c,r2 ? result:0); write=1; break;
    }
    case 0x0c: result=a|b; goto logic;
    case 0x0d: result=a&b; goto logic;
    case 0x0e: result=a^b; goto logic;
    case 0x0f: result=~b; goto logic;
    case 0x10: result=(uint32_t)((int32_t)(r1^16)-16); write=1; break;
    case 0x11: result=add(c,a,(uint32_t)((int32_t)(r1^16)-16)); write=1; break;
    case 0x12: result=condition(c,r1&15); write=1; break;
    case 0x13: sub(c,a,(uint32_t)((int32_t)(r1^16)-16)); break;
    case 0x14: case 0x15: case 0x17: result=shift(c,a,r1,op&15); write=1; break;
    case 0x16: c->psw_id=0; c->cycle_deadline=c->cycles; break;
    case 0x18: c->pc=next; vb_trap(c,r1); return;
    case 0x19: vb_reti(c); return;
    case 0x1a: c->halted=1; break;
    case 0x1c:
        switch(r1) {
        case 5: vb_psw_unpack(c,a); c->cycle_deadline=c->cycles; break;
        case 0: case 2: case 25: c->sysreg[r1]=a&~1u; break;
        case 1: case 3: c->sysreg[r1]=a&0x000ff3ffu; break;
        case 24: c->sysreg[r1]=a&2; break;
        default: break; /* reserved and read-only register writes are ignored */
        } break;
    case 0x1d: result=r1==5 ? vb_psw_pack(c):c->sysreg[r1]; write=1; break;
    case 0x1e: c->psw_id=1; break;
    case 0x1f: case 0x3e: vb_interpreter_extended(c,word,imm); c->gpr[0]=0; return;
    case 0x28: result=addr; write=1; break;
    case 0x29: result=add(c,b,(uint32_t)(int32_t)(int16_t)imm); write=1; break;
    case 0x2a: case 0x2b: {
        uint32_t disp=((uint32_t)(word&0x3ff)<<16)|imm;
        if(op==0x2b) c->gpr[31]=next;
        next=pc+(uint32_t)((int32_t)(disp^0x2000000)-0x2000000); break;
    }
    case 0x2c: result=b|imm; goto logic;
    case 0x2d: result=b&imm; goto logic;
    case 0x2e: result=b^imm; goto logic;
    case 0x2f: result=b+((uint32_t)imm<<16); write=1; break;
    case 0x30: result=(uint32_t)(int32_t)(int8_t)c->read8(addr); write=1; break;
    case 0x31: result=(uint32_t)(int32_t)(int16_t)c->read16(addr&~1u); write=1; break;
    case 0x33: case 0x3b: result=c->read32(addr&~3u); write=1; break;
    case 0x34: case 0x3c: c->write8(addr,(uint8_t)a); break;
    case 0x35: case 0x3d: c->write16(addr&~1u,(uint16_t)a); break;
    case 0x37: case 0x3f: c->write32(addr&~3u,a); break;
    case 0x38: result=c->read8(addr); write=1; break;
    case 0x39: result=c->read16(addr&~1u); write=1; break;
    case 0x3a:
        addr&=~3u; result=c->read32(addr);
        c->write32(addr,sub(c,a,result)==0 ? c->gpr[30]:result); write=1; break;
    default: vb_exception(c,VB_INVALID_OP_HANDLER,VB_ECODE_INVALID_OP); return;
    }
    goto finish;
logic: sz(c,result); c->psw_ov=0; write=1;
finish:
    if(write && r2) c->gpr[r2]=result;
    c->gpr[0]=0; c->pc=next;
}
void vb_interpreter_fallback(CPUState* c) {
    if (vb_execution.mode==VB_EXEC_NATIVE)
        vb_stub_abort("native-only dispatch miss",c->pc,c->pc);
    if (!c->step_budget || c->cycles>=c->cycle_deadline) { c->yielded=1; return; }
    if (!vb_execution.fallback) vb_execution.first_fallback_pc=c->pc;
    vb_execution.last_fallback_pc=c->pc;
    ++vb_execution.fallback;
    --c->step_budget;
    vb_interpreter_step(c);
}
