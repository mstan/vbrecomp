/* vsu_shadow.h — verified-enhancement HLE shadow for the VSU mixer.
 *
 * PERMITTED HLE (PRINCIPLES.md "Verified-Enhancement HLE Is Allowed"). A
 * higher-fidelity FLOAT re-render of the 6-channel VSU wavetable mix runs
 * ALONGSIDE the canon integer mix, is continuously diff-verified against it,
 * and substitutes into the output ring ONLY after a proven window — reverting
 * loudly (DEGRADED log) the instant it diverges. The canon integer mix stays
 * the authoritative output AND the verify oracle.
 *
 * The fidelity gain: the canon path requantizes each channel's gain to a 4-bit
 * scale (`(envelope*level)>>3 + 1`) and mixes/clips in integer S16. The shadow
 * applies the same waveform, envelope, level and stereo state but accumulates
 * in float with the full-precision gain, avoiding the per-channel requantize
 * step and the intermediate-truncation that the integer mix incurs. It is NOT
 * a different synthesis model — same notes, same envelopes, same timing — so
 * the verifier's envelope-correlation gate holds.
 *
 * Default OFF. With VBRECOMP_AUDIO_SHADOW unset/0 the substitution never
 * engages and the emitted bytes are byte-identical to the canon path.
 *
 * ── Attribution ───────────────────────────────────────────────────
 * Verifier ported from JRickey/gba-recomp (crates/gba-core/src/shadow.rs) via
 * the gbarecomp/snesrecomp ports, © Jrickey, MIT OR Apache-2.0. The VSU float
 * re-render is original to this project. See THIRD_PARTY_ATTRIBUTION.md.
 */
#ifndef VB_VSU_SHADOW_H
#define VB_VSU_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Reset verifier state and re-read the env gate. Call from vb_vsu_init. */
void vb_vsu_shadow_reset(void);

/* True when VBRECOMP_AUDIO_SHADOW enables the shadow (env "1"/"on"/"true").
 * Default OFF. Resolved once, cached. */
bool vb_vsu_shadow_enabled(void);

/* Feed one emitted stereo frame to the shadow.
 *
 *   canon_l/r : the canon integer mix for this sample, post-scale, pre-clip
 *               (the value the canon path would store, in S16 numeric range).
 *   shadow_l/r: the float re-render of the same sample (full-precision gain).
 *
 * Drives the differential verifier. When the shadow is proven (and enabled),
 * writes the shadow's S16-clamped value into out_l/out_r and returns true
 * (caller stores those instead of the canon bytes). Otherwise leaves the
 * outputs untouched and returns false (caller stores the canon bytes —
 * byte-identical).
 *
 * On a proven→paused transition it logs a single DEGRADED line to stderr and
 * stops substituting until re-proven. Never becomes the oracle: the canon
 * pair it is fed remains the ground truth being diffed. */
bool vb_vsu_shadow_substitute(float canon_l, float canon_r,
                              float shadow_l, float shadow_r,
                              int16_t* out_l, int16_t* out_r);

#ifdef __cplusplus
}
#endif

#endif /* VB_VSU_SHADOW_H */
