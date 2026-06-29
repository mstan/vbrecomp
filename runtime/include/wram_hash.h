/* wram_hash.h — per-game-frame WRAM fingerprint ring (Axis-6 whole-session
 * fidelity, past the instruction-level VIP-timing wall).
 *
 * At each GAME_START VIP event (the game-logic frame boundary, which the
 * vipphase gate proved aligns 1:1 with the oracle) both processes snapshot an
 * FNV-1a hash of all 64 KiB of WRAM into this always-on, first-N-from-boot
 * ring. The comparator (tools/wramhash_compare.py) aligns the two rings by
 * GAME_START index and reports the first frame whose WRAM state diverges — so
 * fidelity can be checked through gameplay, long past where per-instruction
 * lockstep (cpuhook) stops at the sub-frame VIP-timing wall.
 *
 * Ring QUERIED by absolute seq (Rule 3), never an armed capture.
 */
#ifndef VB_WRAM_HASH_H
#define VB_WRAM_HASH_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 264-byte record — MUST stay byte-identical to the oracle's recorder
 * (beetle-vb/libretro.cpp vb_wramhash_rec). 64 per-region FNV-1a hashes
 * (1 KiB each) so divergence localizes to a region instead of avalanching
 * the whole frame. */
#define VB_WRAMHASH_REGIONS 64
typedef struct {
    uint64_t seq;                          /* GAME_START index from boot */
    uint32_t fnv[VB_WRAMHASH_REGIONS];     /* per-1KiB-region FNV-1a */
} vb_wramhash_rec;

void vb_wram_hash_record(const uint32_t fnv[VB_WRAMHASH_REGIONS]);
uint64_t vb_wram_hash_head(void);
size_t   vb_wram_hash_read_abs(uint64_t start_abs, vb_wramhash_rec* dst,
                               size_t max_recs, uint64_t* out_head,
                               uint64_t* out_resident_lo);
void     vb_wram_hash_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* VB_WRAM_HASH_H */
