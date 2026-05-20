/* fntrace.c — always-on per-function-entry ring. */
#include "fntrace.h"

#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"

static VBFnTraceEntry* s_ring = NULL;
static uint64_t        s_seq  = 0;
static CPUState*       s_cpu  = NULL;

void vb_fntrace_init(void) {
    if (s_ring) return;
    s_ring = (VBFnTraceEntry*)
        calloc(VB_FNTRACE_RING_CAP, sizeof(VBFnTraceEntry));
    s_seq = 0;
}

void vb_fntrace_shutdown(void) {
    free(s_ring);
    s_ring = NULL;
    s_seq = 0;
    s_cpu = NULL;
}

void vb_fntrace_set_active_cpu(CPUState* cpu) {
    s_cpu = cpu;
}

void vb_fntrace_reset(void) {
    s_seq = 0;
}

uint64_t vb_fntrace_seq(void)      { return s_seq; }
size_t   vb_fntrace_capacity(void) { return VB_FNTRACE_RING_CAP; }

void vb_fntrace_record(uint32_t pc, uint32_t lp) {
    if (!s_ring) return;
    VBFnTraceEntry* slot = &s_ring[s_seq % VB_FNTRACE_RING_CAP];
    slot->seq   = s_seq;
    slot->cycle = s_cpu ? s_cpu->cycles : 0;
    slot->pc    = pc;
    slot->lp    = lp;
    s_seq++;
}

size_t vb_fntrace_query(const VBFnTraceFilter* f,
                        VBFnTraceEntry* out, size_t max) {
    if (!s_ring || !out || !max || !f) return 0;

    const uint64_t available = s_seq;
    uint64_t lo = f->from_seq;
    uint64_t hi = (f->to_seq == 0 || f->to_seq > available) ? available
                                                            : f->to_seq;
    if (available > VB_FNTRACE_RING_CAP &&
        lo < available - VB_FNTRACE_RING_CAP) {
        lo = available - VB_FNTRACE_RING_CAP;
    }
    if (hi <= lo) return 0;

    const uint32_t cap = f->max_results ? f->max_results : 0xFFFFFFFFu;
    size_t n = 0;
    for (uint64_t s = lo; s < hi && n < max && n < cap; ++s) {
        const VBFnTraceEntry e = s_ring[s % VB_FNTRACE_RING_CAP];
        if (e.pc < f->pc_min)                      continue;
        if (f->pc_max && e.pc >= f->pc_max)        continue;
        if (e.lp < f->lp_min)                      continue;
        if (f->lp_max && e.lp >= f->lp_max)        continue;
        out[n++] = e;
    }
    return n;
}
