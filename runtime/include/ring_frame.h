/* ring_frame.h — 36k-frame snapshot ring.
 *
 * Always-on per ~/.claude/CLAUDE.md ring-buffer rule. Sized for
 * ~12 minutes at 50.27 Hz. Eviction is modular; the seq counter is
 * 64-bit so absolute ordering across evictions is reconstructable.
 *
 * Phase 1 records only the basics (PC, frame, last function name).
 * Phase 4+ adds VIP/VSU/IRQ snapshots.
 */
#ifndef VB_RING_FRAME_H
#define VB_RING_FRAME_H

#include <stdint.h>
#include <stddef.h>   /* size_t (not transitively pulled in by clang) */
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

#define VB_FRAME_RING_CAP 36000u

typedef struct VBFrameRecord {
    uint64_t seq;
    uint32_t frame_idx;
    uint32_t pc;
    uint32_t gpr_snapshot[32];   /* full GPRs */
    uint32_t sysreg_psw;
    /* Phase 4+ extends: VIP state, VSU state, pad, last function. */
} VBFrameRecord;

void vb_ring_frame_init(void);
void vb_ring_frame_shutdown(void);

/* Record one frame. Called at the end of the runtime's main loop tick. */
void vb_ring_frame_record(const CPUState* cpu);

/* Snapshot query for the debug server. Returns the number of records
 * copied (≤ max). `from_seq` selects the starting absolute seq. */
size_t vb_ring_frame_dump(uint64_t from_seq, VBFrameRecord* out, size_t max);

uint64_t vb_ring_frame_seq(void);

#ifdef __cplusplus
}
#endif

#endif /* VB_RING_FRAME_H */
