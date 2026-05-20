/* ring_frame.c — 36k-frame snapshot ring.
 *
 * Allocated once at init; eviction is modular index. seq counter is
 * 64-bit so absolute ordering survives wrap.
 */
#include "ring_frame.h"

#include <stdlib.h>
#include <string.h>

static VBFrameRecord* s_ring = NULL;
static uint64_t s_seq = 0;

void vb_ring_frame_init(void) {
    if (s_ring) return;
    s_ring = (VBFrameRecord*)calloc(VB_FRAME_RING_CAP, sizeof(VBFrameRecord));
    s_seq = 0;
}

void vb_ring_frame_shutdown(void) {
    free(s_ring);
    s_ring = NULL;
    s_seq = 0;
}

void vb_ring_frame_record(const CPUState* cpu) {
    if (!s_ring || !cpu) return;
    VBFrameRecord* slot = &s_ring[s_seq % VB_FRAME_RING_CAP];
    slot->seq = s_seq;
    slot->frame_idx = cpu->frame;
    slot->pc = cpu->pc;
    memcpy(slot->gpr_snapshot, cpu->gpr, sizeof(slot->gpr_snapshot));
    slot->sysreg_psw = cpu->sysreg[VB_SR_PSW];
    s_seq++;
}

size_t vb_ring_frame_dump(uint64_t from_seq, VBFrameRecord* out, size_t max) {
    if (!s_ring || !out || max == 0) return 0;
    uint64_t available = s_seq;
    if (from_seq >= available) return 0;

    /* Don't try to read evicted history. */
    if (available > VB_FRAME_RING_CAP &&
        from_seq < available - VB_FRAME_RING_CAP) {
        from_seq = available - VB_FRAME_RING_CAP;
    }

    size_t n = 0;
    for (uint64_t s = from_seq; s < available && n < max; ++s, ++n) {
        out[n] = s_ring[s % VB_FRAME_RING_CAP];
    }
    return n;
}

uint64_t vb_ring_frame_seq(void) { return s_seq; }
