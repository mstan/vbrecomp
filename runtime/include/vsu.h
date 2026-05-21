/* vsu.h — Virtual Sound Unit surface.
 *
 * 6-channel V810 sound synthesis. Ports beetle-vb/mednafen/vb/vsu.c
 * (lines 26-547) with the Blip_Synth band-limited path replaced by
 * a direct stereo S16 sample queue suitable for SDL_QueueAudio.
 *
 * Channel layout:
 *   ch 0..3 : 32 x 6-bit waveform from WaveData[ram_addr]
 *   ch 4    : waveform + frequency sweep / FM modulation via ModData
 *   ch 5    : 15-bit LFSR noise
 *
 * Audio output is paced by `vb_vsu_tick(cpu_cycles)` from main.cpp:
 * every VSU_CYCLES_PER_SAMPLE V810 cycles produces one stereo S16
 * frame in the internal ring. SDL drains via `vb_vsu_pull_samples`.
 */
#ifndef VB_VSU_H
#define VB_VSU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VSU_OUTPUT_HZ          44100
#define VSU_CPU_HZ             20000000
/* 20 000 000 / 44 100 = 453.51 — Bresenham accumulator keeps drift
 * < 1 cycle per output sample, well below audible threshold. */
#define VSU_CYCLES_PER_SAMPLE  453

void vb_vsu_init(void);
void vb_vsu_shutdown(void);

uint8_t  vb_vsu_read8 (uint32_t addr);
uint16_t vb_vsu_read16(uint32_t addr);
uint32_t vb_vsu_read32(uint32_t addr);
void     vb_vsu_write8 (uint32_t addr, uint8_t  v);
void     vb_vsu_write16(uint32_t addr, uint16_t v);
void     vb_vsu_write32(uint32_t addr, uint32_t v);

/* Advance VSU state by `cpu_cycles` V810 cycles. Generates stereo
 * S16 frames into the internal ring; SDL pulls them with
 * `vb_vsu_pull_samples`. */
void vb_vsu_tick(uint64_t cpu_cycles);

/* Drain up to `n_frames` stereo S16 frames into `dst` (interleaved
 * L, R, L, R, ...). Returns the number of frames actually produced;
 * when the ring is empty, the remainder of `dst` is filled with 0
 * silence to avoid SDL underrun clicks. */
size_t vb_vsu_pull_samples(int16_t* dst, size_t n_frames);

/* Number of stereo frames currently buffered. */
size_t vb_vsu_pending_frames(void);

#ifdef __cplusplus
}
#endif

#endif /* VB_VSU_H */
