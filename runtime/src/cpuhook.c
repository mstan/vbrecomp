/* cpuhook.c — recomp-side per-instruction CPU-hook ring.
 *
 * See cpuhook.h. vb_cpuhook_record() is called once per retired guest
 * instruction (from generated code, only when built -DVB_CPUHOOK_ENABLE)
 * and stores {pc, packed PSW, regs-FNV, cycle} into a power-of-two ring.
 * The debug server streams a window by absolute seq (cpuhook command,
 * port 4390), mirroring the oracle's cpuhook on 4391. This is the recomp
 * half of the first-divergence + cycle-Delta harness (Axes 1/2/3/6).
 */
#include "cpuhook.h"

/* FIRST-N-from-boot capture: fills from instruction 0 and FREEZES when
 * full (does not wrap). The recomp free-runs ~30x realtime headless and
 * the pause command does not gate dispatch, so a wrapping (last-N) ring
 * would evict the boot window before a comparator could drain it.
 * Freezing at capacity keeps [0, capacity) always resident for the
 * first-divergence diff. 8M entries * 24B = 192 MiB. Matches the oracle. */
#define CPUHOOK_RING (1u << 23)

static vb_cpuhook_rec s_ring[CPUHOOK_RING];
static uint64_t       s_total;   /* retired-instruction count (caps at RING) */

void vb_cpuhook_record(const CPUState* cpu) {
    if (s_total >= CPUHOOK_RING) return;   /* frozen: boot window full */

    /* FNV-1a over gpr[1..30]. r0 hardwired 0; r31 (lp) EXCLUDED — the
     * runtime seeds it to the 0xDEAD0000 top-level-return sentinel at reset
     * (main.cpp), which the oracle does not have until the cart's first
     * JAL; lp errors surface as PC (wrong-return) divergences instead. Must
     * match the oracle hash (libretro.cpp vb_oracle_cpuhook). */
    uint32_t h = 2166136261u;
    for (int i = 1; i < 31; ++i) { h = (h ^ cpu->gpr[i]) * 16777619u; }

    vb_cpuhook_rec* e = &s_ring[s_total];
    e->pc    = cpu->pc;
    e->psw   = vb_psw_pack(cpu);
    e->fnv   = h;
    e->pad   = 0;
    e->cycle = cpu->cycles;   /* pre-charge cycle (macro fires before +=) */
    s_total++;
}

uint64_t vb_cpuhook_head(void) {
    return s_total;
}

size_t vb_cpuhook_read_abs(uint64_t start_abs, vb_cpuhook_rec* dst,
                           size_t max_recs, uint64_t* out_head,
                           uint64_t* out_resident_lo) {
    const uint64_t head = s_total;
    /* First-N capture never wraps, so nothing is ever evicted. */
    if (out_head)        *out_head        = head;
    if (out_resident_lo) *out_resident_lo = 0;
    if (!dst || !max_recs) return 0;

    uint64_t cur = start_abs;
    size_t produced = 0;
    while (produced < max_recs && cur < head) {
        dst[produced++] = s_ring[cur];
        cur++;
    }
    return produced;
}
