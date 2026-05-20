/* wtrace.c — always-on per-store ring.
 *
 * See runtime/include/wtrace.h for the design rationale. Single-
 * threaded execution model (the recompiled cart runs on the main
 * thread, TCP poll is interleaved sequentially), so the ring needs
 * no locks.
 */
#include "wtrace.h"

#include <stdlib.h>
#include <string.h>

#include "cpu_state.h"

static VBWTraceEntry* s_ring = NULL;
static uint64_t       s_seq  = 0;
static CPUState*      s_cpu  = NULL;

void vb_wtrace_init(void) {
    if (s_ring) return;
    s_ring = (VBWTraceEntry*)calloc(VB_WTRACE_RING_CAP, sizeof(VBWTraceEntry));
    s_seq = 0;
}

void vb_wtrace_shutdown(void) {
    free(s_ring);
    s_ring = NULL;
    s_seq = 0;
    s_cpu = NULL;
}

void vb_wtrace_set_active_cpu(CPUState* cpu) {
    s_cpu = cpu;
}

void vb_wtrace_reset(void) {
    s_seq = 0;
}

uint64_t vb_wtrace_seq(void)      { return s_seq; }
size_t   vb_wtrace_capacity(void) { return VB_WTRACE_RING_CAP; }

void vb_wtrace_record(uint32_t addr, uint32_t value, uint8_t width) {
    if (!s_ring) return;
    VBWTraceEntry* slot = &s_ring[s_seq % VB_WTRACE_RING_CAP];
    slot->seq      = s_seq;
    slot->cycle    = s_cpu ? s_cpu->cycles : 0;
    slot->pc       = s_cpu ? s_cpu->pc     : 0;
    slot->addr     = addr;
    slot->value    = value;
    slot->width    = width;
    slot->region   = (uint8_t)(((addr & 0x07FFFFFFu) >> 24) & 0x7u);
    slot->reserved = 0;
    s_seq++;
}

size_t vb_wtrace_query(const VBWTraceFilter* f,
                       VBWTraceEntry* out, size_t max) {
    if (!s_ring || !out || !max || !f) return 0;

    const uint64_t available = s_seq;
    uint64_t lo = f->from_seq;
    uint64_t hi = (f->to_seq == 0 || f->to_seq > available) ? available
                                                            : f->to_seq;
    /* Clamp to live window (entries before this seq have been evicted). */
    if (available > VB_WTRACE_RING_CAP &&
        lo < available - VB_WTRACE_RING_CAP) {
        lo = available - VB_WTRACE_RING_CAP;
    }
    if (hi <= lo) return 0;

    const uint32_t cap = f->max_results ? f->max_results : 0xFFFFFFFFu;
    size_t n = 0;

    for (uint64_t s = lo; s < hi && n < max && n < cap; ++s) {
        const VBWTraceEntry e = s_ring[s % VB_WTRACE_RING_CAP];
        if (e.addr < f->addr_min)                       continue;
        if (f->addr_max && e.addr >= f->addr_max)       continue;
        if (e.pc   < f->pc_min)                         continue;
        if (f->pc_max && e.pc   >= f->pc_max)           continue;
        out[n++] = e;
    }
    return n;
}
