/* wram_hash.c — see wram_hash.h. First-N-from-boot ring (mirrors
 * vip_phase.c); observability only, does not touch emulation. */
#include "wram_hash.h"
#include <string.h>

#define VB_WRAM_HASH_RING (1u << 15)   /* 32768 game frames ~ 11 min */

static vb_wramhash_rec s_ring[VB_WRAM_HASH_RING];
static uint64_t        s_total;

void vb_wram_hash_reset(void) {
    s_total = 0;
}

void vb_wram_hash_record(const uint32_t fnv[VB_WRAMHASH_REGIONS]) {
    if (s_total >= VB_WRAM_HASH_RING) return;   /* first-N: frozen when full */
    vb_wramhash_rec* e = &s_ring[s_total];
    e->seq = s_total;
    memcpy(e->fnv, fnv, sizeof(e->fnv));
    s_total++;
}

uint64_t vb_wram_hash_head(void) { return s_total; }

size_t vb_wram_hash_read_abs(uint64_t start_abs, vb_wramhash_rec* dst,
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
