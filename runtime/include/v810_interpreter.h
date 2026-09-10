#ifndef VB_V810_INTERPRETER_H
#define VB_V810_INTERPRETER_H
#include "cpu_state.h"
#ifdef __cplusplus
extern "C" {
#endif
enum { VB_EXEC_HYBRID, VB_EXEC_NATIVE, VB_EXEC_INTERPRETER };
typedef struct {
    uint64_t interpreted, fallback, native_entries;
    uint32_t first_fallback_pc, last_fallback_pc;
    int mode;
    uint64_t native_instructions, step_remaining;
    uint32_t breakpoint_pc;
    int stepping, breakpoint_enabled, stopped, skip_break;
} VbExecutionStats;
extern VbExecutionStats vb_execution;
/* Virtual Boy has a 16-bit external bus in every region. Preserve load/store
 * pipeline state across native calls, interpreter handoffs and device yields.
 * Reference: Beetle v810_oploop.inc LD/ST/IN/OUT handlers and lastop updates. */
static inline void vb_execution_charge(CPUState* c, unsigned op, unsigned base) {
    unsigned access_cost=base;
    int previous=c->pipeline_class;
    c->pipeline_class=0;
    if (op==0x30 || op==0x31 || op==0x33) {
        if(previous>=0) base+=(previous==1 ? 1:2)+(op==0x33 ? 2:0);
        c->pipeline_class=1;
    } else if(op==0x34 || op==0x35 || op==0x37) {
        if(previous==2) base+=op==0x37 ? 3:1;
        c->pipeline_class=2;
    } else if(op==0x38 || op==0x39 || op==0x3b) {
        if(op==0x3b) { base+=2;access_cost+=2; }
        c->pipeline_class=3;
    } else if(op==0x3c || op==0x3d || op==0x3f) {
        if(previous==4) base+=op==0x3f ? 3:1;
        c->pipeline_class=4;
    } else if((op>=8 && op<=11) || op==0x3e) c->pipeline_class=-1;
    c->cycles+=base;
    c->bus_tail_cycles=(uint8_t)(base-access_cost);
}
/* Execute one real instruction through the live bus. No host call stack. */
void vb_interpreter_step(CPUState* cpu);
void vb_interpreter_fallback(CPUState* cpu);
void vb_interpreter_extended(CPUState* cpu, uint16_t first, uint16_t second);
int vb_execution_debug_check(CPUState* cpu);
static inline int vb_execution_before(CPUState* cpu) {
    return (vb_execution.stepping || vb_execution.breakpoint_enabled) && vb_execution_debug_check(cpu);
}
#ifdef __cplusplus
}
#endif
#endif
