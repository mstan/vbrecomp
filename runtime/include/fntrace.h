/* fntrace.h — always-on per-function-entry ring buffer.
 *
 * Every entry into a recompiled function body records (pc, lp, cycle)
 * into a fixed-size ring. Probes query the ring; nothing is ever
 * "armed" — see ~/.claude/CLAUDE.md global ring-buffer rule.
 *
 * Capacity is 256K entries (~12 MB). At a few thousand calls per
 * frame of cart execution this gives several seconds of rolling
 * history — long enough to trace back through the boot path or the
 * last hundred ISR cycles.
 *
 * Hook site: the codegen inserts vb_fntrace_record() at the
 * "fresh-call" path of each emitted function body (after the leader-
 * routing switch, before cpu->pc is set to fn.start_pc). Yielded
 * resumes do NOT generate a record because they take a case-branch
 * out of the switch and skip the recorder.
 *
 * For single-leader functions there is no routing switch and every
 * entry — fresh or resumed — is recorded; downstream analysis can
 * collapse repeat-same-pc runs into a single call if it cares.
 */
#ifndef VB_FNTRACE_H
#define VB_FNTRACE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct CPUState;

#define VB_FNTRACE_RING_CAP (1u << 18)   /* 262,144 entries, ~12 MB */

typedef struct VBFnTraceEntry {
    uint64_t seq;       /* absolute index since process start */
    uint64_t cycle;     /* cpu->cycles at entry */
    uint32_t pc;        /* function start PC (the body being entered) */
    uint32_t lp;        /* gpr[31] — caller's return address */
} VBFnTraceEntry;

void vb_fntrace_init(void);
void vb_fntrace_shutdown(void);

void vb_fntrace_set_active_cpu(struct CPUState* cpu);

/* Recorder. Called from the head of each emitted function body. */
void vb_fntrace_record(uint32_t pc, uint32_t lp);

void vb_fntrace_reset(void);

uint64_t vb_fntrace_seq(void);
size_t   vb_fntrace_capacity(void);

typedef struct VBFnTraceFilter {
    uint64_t from_seq;
    uint64_t to_seq;       /* 0 = current seq */
    uint32_t pc_min;
    uint32_t pc_max;       /* 0 = no max */
    uint32_t lp_min;
    uint32_t lp_max;       /* 0 = no max */
    uint32_t max_results;
} VBFnTraceFilter;

size_t vb_fntrace_query(const VBFnTraceFilter* f,
                        VBFnTraceEntry* out, size_t max);

#ifdef __cplusplus
}
#endif

#endif /* VB_FNTRACE_H */
