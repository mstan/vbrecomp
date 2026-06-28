/* cpuhook.h — recomp-side per-instruction CPU-hook ring.
 *
 * The recomp half of the recomp-vs-oracle first-divergence + cycle-Delta
 * harness (Axes 1/2/3/6). When the runtime is built with
 * -DVB_CPUHOOK_ENABLE, the emitter's per-instruction VB_CPUHOOK(cpu)
 * (see cpu_state.h) calls vb_cpuhook_record() once per retired guest
 * instruction. The record format is byte-identical to the oracle's
 * vb_cpuhook_rec (beetle-vb/libretro.cpp) so a single comparator can
 * unpack both. Always-on ring, queried by absolute retire-seq — never an
 * armed capture (CLAUDE.md Rule 3).
 */
#ifndef VB_CPUHOOK_H
#define VB_CPUHOOK_H

#include <stddef.h>
#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* MUST match the oracle's vb_cpuhook_rec layout (24 bytes):
 * pc, psw, fnv, pad : uint32 ; cycle : uint64. */
typedef struct {
    uint32_t pc;
    uint32_t psw;     /* vb_psw_pack(cpu) — same bit layout as oracle S_REG[PSW] */
    uint32_t fnv;     /* FNV-1a over gpr[1..31] (gpr[0] hardwired 0) */
    uint32_t pad;
    uint64_t cycle;   /* cpu->cycles BEFORE this instruction's charge */
} vb_cpuhook_rec;

/* Record one retired instruction (called from generated code via the
 * VB_CPUHOOK macro). No-op overhead is avoided at the call site: the
 * macro expands to nothing unless VB_CPUHOOK_ENABLE is defined. */
void vb_cpuhook_record(const CPUState* cpu);

/* Monotonic count of instructions ever recorded. */
uint64_t vb_cpuhook_head(void);

/* Copy up to max_recs records starting at absolute seq start_abs into dst.
 * Reports the head and the oldest still-resident seq (so a lagging
 * consumer sees a gap, never a silent skip). Returns the count copied. */
size_t vb_cpuhook_read_abs(uint64_t start_abs, vb_cpuhook_rec* dst,
                           size_t max_recs, uint64_t* out_head,
                           uint64_t* out_resident_lo);

#ifdef __cplusplus
}
#endif

#endif /* VB_CPUHOOK_H */
