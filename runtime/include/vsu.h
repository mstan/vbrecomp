/* vsu.h — Virtual Sound Unit surface.
 *
 * 6-channel V810 sound synthesis. Ports beetle-vb/mednafen/vb/vsu.c
 * (lines 26-547) INCLUDING the Blip_Synth band-limited output stage:
 * channels feed amplitude deltas into a 5 MHz Blip_Buffer that drains
 * 44.1 kHz stereo S16 frames into the internal ring (Axis-5b — see
 * docs/AXIS5B_BLIP_OUTPUT.md). SDL drains via `vb_vsu_pull_samples`.
 *
 * Channel layout:
 *   ch 0..3 : 32 x 6-bit waveform from WaveData[ram_addr]
 *   ch 4    : waveform + frequency sweep / FM modulation via ModData
 *   ch 5    : 15-bit LFSR noise
 *
 * Output is driven by `vb_vsu_tick(cpu_cycles)` from main.cpp; the Blip
 * clock-rate factor (not a cycle-per-sample cadence) sets the 44.1 kHz
 * rate, so output stays sample-rate-locked independent of tick size.
 */
#ifndef VB_VSU_H
#define VB_VSU_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VSU_OUTPUT_HZ          44100

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
void vb_vsu_end_frame(void);

/* Drain up to `n_frames` stereo S16 frames into `dst` (interleaved
 * L, R, L, R, ...). Returns the number of frames actually produced;
 * when the ring is empty, the remainder of `dst` is filled with 0
 * silence to avoid SDL underrun clicks. */
size_t vb_vsu_pull_samples(int16_t* dst, size_t n_frames);

/* Number of stereo frames currently buffered. */
size_t vb_vsu_pending_frames(void);

/* ---- Always-on capture tap (accuracy oracle diff) -----------------
 *
 * The output ring is written from boot, one stereo frame per emitted
 * sample, and `vb_vsu_total_frames()` is the monotonic count of frames
 * ever emitted (never reset except by vb_vsu_init). The capture read
 * below addresses frames by ABSOLUTE index, independent of the SDL
 * consumer's drain cursor (`vb_vsu_pull_samples`), so a debug-server
 * probe can stream the always-on history without disturbing playback.
 *
 * This is the ring-query model (CLAUDE.md Rule 3 / global ring rule):
 * the probe asks for the window [start_abs, head) it cares about; it
 * never arms a capture. Frames older than the ring capacity have been
 * overwritten — `*out_resident_lo` reports the oldest still-readable
 * absolute index so the caller can detect (and never silently skip) a
 * gap if it falls behind.
 *
 * Returns the number of stereo frames copied into `dst` (interleaved
 * L,R). `dst` must hold at least `max_frames * 2` int16. */
uint64_t vb_vsu_total_frames(void);
size_t   vb_vsu_read_abs(uint64_t start_abs, int16_t* dst,
                         size_t max_frames,
                         uint64_t* out_head_abs,
                         uint64_t* out_resident_lo);
unsigned vb_vsu_output_hz(void);

#ifdef __cplusplus
}
#endif

#endif /* VB_VSU_H */
