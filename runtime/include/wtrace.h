/* wtrace.h — always-on per-store ring buffer.
 *
 * Every store executed by the recompiled cart (and every store the bus
 * routes through vb_write8/16/32) is recorded here, unconditionally,
 * from the moment the runtime is initialised. This is the canonical
 * implementation of the "always-on ring buffer" pattern from
 * ~/.claude/CLAUDE.md (global rule): probes QUERY the ring, never ARM
 * a capture and then run a workload.
 *
 * Capacity is 1M entries (~24 MB). At ~1M stores per second of cart
 * execution this gives a ~1s rolling window; for the cold-boot work
 * the WORLDS region is touched within the first few thousand stores,
 * so 1M is comfortable headroom and the wrap-around behaviour is
 * documented so future probes know what to expect.
 *
 * Hook site: runtime/src/memory.c vb_write{8,16,32}() call
 * vb_wtrace_record() at the top of each function, BEFORE address
 * decode, so even writes that subsequently stub_abort (e.g. into
 * cart ROM) are visible. Source PC is read from the currently-active
 * CPU pointer set by main.cpp via vb_wtrace_set_active_cpu(). The
 * emitter sets cpu->pc to the address of every executed instruction
 * before it runs, so the PC in the record points at the store
 * instruction itself.
 */
#ifndef VB_WTRACE_H
#define VB_WTRACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

#define VB_WTRACE_RING_CAP (1u << 20)   /* 1,048,576 entries, ~24 MB */

typedef struct VBWTraceEntry {
    uint64_t seq;       /* absolute index since process start */
    uint64_t cycle;     /* cpu->cycles at write time */
    uint32_t pc;        /* source PC of the store instruction */
    uint32_t addr;      /* virtual address as the cart asked for it */
    uint32_t value;     /* zero-extended value written */
    uint8_t  width;     /* 1, 2, or 4 bytes */
    uint8_t  region;    /* (folded addr >> 24) & 7 — fast filter shortcut */
    uint16_t reserved;
} VBWTraceEntry;

void vb_wtrace_init(void);
void vb_wtrace_shutdown(void);

/* Wire the CPU whose pc/cycles the recorder should sample. The runtime
 * has exactly one CPUState, so this is set once at startup and not
 * touched again. */
void vb_wtrace_set_active_cpu(struct CPUState* cpu);

/* Recorder. Called from vb_write{8,16,32}. width is 1/2/4. The recorder
 * is unconditional — there is no "armed" state, no filter, nothing to
 * configure at probe time. Filtering happens in vb_wtrace_query(). */
void vb_wtrace_record(uint32_t addr, uint32_t value, uint8_t width);

/* Manual seq reset. NOT needed for normal operation — the ring is
 * always on. Provided so a long-running session can establish a fresh
 * "from now on" baseline if every probe so far has been cumulative. */
void vb_wtrace_reset(void);

uint64_t vb_wtrace_seq(void);
size_t   vb_wtrace_capacity(void);

typedef struct VBWTraceFilter {
    uint64_t from_seq;     /* inclusive; 0 = start of available window */
    uint64_t to_seq;       /* exclusive; 0 = current seq */
    uint32_t addr_min;     /* inclusive; 0 = no min */
    uint32_t addr_max;     /* exclusive; 0 = no max (full 32-bit) */
    uint32_t pc_min;       /* inclusive; 0 = no min */
    uint32_t pc_max;       /* exclusive; 0 = no max */
    uint32_t max_results;  /* 0 = unbounded (caller still bounded by out[]) */
} VBWTraceFilter;

size_t vb_wtrace_query(const VBWTraceFilter* f,
                       VBWTraceEntry* out, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* VB_WTRACE_H */
