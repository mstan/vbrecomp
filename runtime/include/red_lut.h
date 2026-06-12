/* red_lut.h — present-time red-LED intensity → RGB simulation.
 *
 * PRESENT-TIME ONLY. This never touches emulation, the VIP framebuffer the
 * state machine produces, or any differential-verify / oracle path —
 * vb-beetle comparisons stay defined on the RAW red-channel bytes the VIP
 * renderer emits. The LUT is a 256-entry table applied at framebuffer-pack
 * time (one indexed lookup per pixel), keyed on the SAME 0..255 brightness
 * scalar (`bv`) the canon renderer already computes from the BRT cache.
 *
 * It defaults to Raw (exact passthrough — byte-identical to the current
 * `0xFF000000 | gamma(bv)<<16` output, G=B=0), so default behaviour and every
 * hashed/verified frame are unchanged unless a model is opted in via
 * VBRECOMP_SCREEN={raw,led,led_warm}.
 *
 * Why a red-channel LUT and NOT a GBA-style BGR555 colorimetry LUT: the
 * Virtual Boy display is a RED-LED MONOCHROME scanned-mirror unit — each
 * pixel is a single red-intensity level (2 bits → BRT cache → 0..255), not an
 * RGB triple. There is no color gamut to map. The appropriate present-time
 * model is a red-LED intensity/gamma curve, optionally with a small warm
 * phosphor spill so high intensities read as the real unit's slightly
 * orange-red rather than pure primary red. The display's LED PERSISTENCE
 * (slow decay producing motion smear) and the STEREOSCOPIC dual-eye path are
 * NOT modelled here — see docs/SHADOW_ENHANCEMENTS.md ("Out of scope"); we do
 * not fake hardware behaviour we have not measured.
 *
 * ── Attribution ───────────────────────────────────────────────────
 * Structure mirrors the gbarecomp present-time color LUT
 * (src/runtime/color_lut.{h,cpp}, itself ported from JRickey/gba-recomp
 * crates/screen, © Jrickey, MIT OR Apache-2.0). The red-LED intensity model
 * and the 1-channel apply path are original to this project (the GBA
 * BGR555/CIE-gamut core does not apply to a monochrome red display).
 * See THIRD_PARTY_ATTRIBUTION.md.
 */
#ifndef VB_RED_LUT_H
#define VB_RED_LUT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Which present-time red-LED model to simulate. */
typedef enum {
  VB_SCREEN_RAW = 0,      /* gamma-only red channel, G=B=0 (default; passthrough) */
  VB_SCREEN_LED,          /* red-LED gamma, pure red primary */
  VB_SCREEN_LED_WARM,     /* red-LED gamma + small warm phosphor spill into G */
} VbScreenKind;

/* Parse a config/env token; returns false (and leaves *out) if unrecognized. */
bool vb_screen_kind_from_name(const char* name, VbScreenKind* out);

/* Resolve the active model from VBRECOMP_SCREEN (env). Defaults to RAW when
 * unset or unrecognized. Cached after the first call. */
VbScreenKind vb_red_lut_active_kind(void);

/* True when the active model is the exact-passthrough default — callers may
 * use this to skip the per-pixel apply entirely and stay byte-identical. */
bool vb_red_lut_is_passthrough(void);

/* Map one 0..255 brightness scalar to a packed ARGB8888 pixel (0xFFrrggbb)
 * under the active model. RAW reproduces the canon renderer's output exactly:
 * the input `bv` is the gamma-corrected red value, emitted as 0xFF000000 |
 * (bv<<16). The table is built lazily on first use. */
uint32_t vb_red_lut_map(int bv);

#ifdef __cplusplus
}
#endif

#endif /* VB_RED_LUT_H */
