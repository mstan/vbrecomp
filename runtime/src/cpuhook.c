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

/* Continuous 8M-entry history. Absolute sequences expose eviction; pause
 * and bounded frame control let clients drain any desired interval. */
#define CPUHOOK_RING (1u << 23)

static vb_cpuhook_rec s_ring[CPUHOOK_RING];
static uint64_t       s_total;   /* absolute boundary count */

void vb_cpuhook_record(const CPUState* cpu) {

    /* Version 2 hashes all writable GPRs, including the architectural LP. */
    uint32_t h = 2166136261u;
    for (int i = 1; i < 32; ++i) { h = (h ^ cpu->gpr[i]) * 16777619u; }

    vb_cpuhook_rec* e = &s_ring[s_total & (CPUHOOK_RING-1)];
    e->pc    = cpu->pc;
    e->psw   = vb_psw_pack(cpu);
    e->fnv   = h;
    e->pad   = 2;
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
    const uint64_t lo=head>CPUHOOK_RING ? head-CPUHOOK_RING:0;
    if (out_head)        *out_head        = head;
    if (out_resident_lo) *out_resident_lo = lo;
    if (!dst || !max_recs) return 0;

    uint64_t cur = start_abs<lo ? lo:start_abs;
    size_t produced = 0;
    while (produced < max_recs && cur < head) {
        dst[produced++] = s_ring[cur & (CPUHOOK_RING-1)];
        cur++;
    }
    return produced;
}
