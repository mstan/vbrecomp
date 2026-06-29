/* vip_phase.c — see vip_phase.h.
 *
 * First-N-from-boot ring (mirrors cpuhook.c): fills from event 0, freezes
 * when full so the boot window is never evicted. Recorded unconditionally
 * (cheap: a handful of events per frame) — it is observability that does not
 * touch emulation state, so the faithful path is byte-identical.
 */
#include "vip_phase.h"

#define VB_VIP_PHASE_RING (1u << 16)   /* 65536 events ~ 13k frames ~ 4 min */

static vb_vipphase_rec s_ring[VB_VIP_PHASE_RING];
static uint64_t        s_total;

void vb_vip_phase_reset(void) {
    s_total = 0;
}

void vb_vip_phase_record(uint16_t event, uint16_t dpstts, uint16_t xpstts) {
    if (s_total >= VB_VIP_PHASE_RING) return;   /* first-N: frozen when full */
    vb_vipphase_rec* e = &s_ring[s_total];
    e->seq      = s_total;
    e->event    = event;
    e->dpstts   = dpstts;
    e->xpstts   = xpstts;
    e->reserved = 0;
    s_total++;
}

uint64_t vb_vip_phase_head(void) { return s_total; }

size_t vb_vip_phase_read_abs(uint64_t start_abs, vb_vipphase_rec* dst,
                             size_t max_recs, uint64_t* out_head,
                             uint64_t* out_resident_lo) {
    if (out_head)        *out_head        = s_total;
    if (out_resident_lo) *out_resident_lo = 0;   /* first-N never evicts */
    if (!dst || !max_recs) return 0;
    uint64_t cur = start_abs;
    size_t n = 0;
    while (n < max_recs && cur < s_total) {
        dst[n++] = s_ring[cur];
        cur++;
    }
    return n;
}
