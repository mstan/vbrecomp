/* vip_phase.h — VIP draw-timing phase event ring (Axis-5a phase gate).
 *
 * At each deterministic VIP draw-timing event (the INTPND-raise points:
 * FRAME_START / GAME_START / XP_END / L/R FB_END) the VIP state machine
 * snapshots its display status (DPSTTS) and drawing status (XPSTTS) into
 * this always-on, first-N-from-boot ring. The oracle (beetle-vb) records a
 * byte-identical ring at the same events, so one comparator
 * (tools/vipphase_compare.py) can align the two by event sequence and diff
 * the phase WITHOUT cycle-sampling noise — both VIPs are freshly advanced at
 * an event boundary, so the snapshot is well-defined.
 *
 * Always-on ring QUERIED by absolute seq (CLAUDE.md Rule 3) — never an armed
 * capture. First-N-from-boot: freezes when full so the boot window (where
 * the cpuhook's first draw-timing divergence lives) is never evicted.
 */
#ifndef VB_VIP_PHASE_H
#define VB_VIP_PHASE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 16-byte record — MUST stay byte-identical to the oracle's recorder
 * (beetle-vb/libretro.cpp vb_vipphase_rec) so a single comparator unpacks
 * both. Alignment is by `seq`; the comparator derives a frame index by
 * counting FRAME_START events. */
typedef struct {
    uint64_t seq;       /* event index from boot */
    uint16_t event;     /* the INTPND bit raised at this snapshot */
    uint16_t dpstts;    /* VIP display status (DPSTTS) at the event */
    uint16_t xpstts;    /* VIP drawing status (XPSTTS) at the event */
    uint16_t reserved;
} vb_vipphase_rec;

/* Snapshot one VIP draw-timing event. Called from vip.c immediately after
 * an INTPND bit is raised, with DPSTTS/XPSTTS computed at that instant. */
void vb_vip_phase_record(uint16_t event, uint16_t dpstts, uint16_t xpstts);

uint64_t vb_vip_phase_head(void);
size_t   vb_vip_phase_read_abs(uint64_t start_abs, vb_vipphase_rec* dst,
                               size_t max_recs, uint64_t* out_head,
                               uint64_t* out_resident_lo);
void     vb_vip_phase_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* VB_VIP_PHASE_H */
