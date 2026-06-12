/* red_lut.c — see red_lut.h. Present-time only.
 *
 * The red-LED intensity model is original to this project; the present-LUT
 * structure mirrors gbarecomp's color_lut. See THIRD_PARTY_ATTRIBUTION.md.
 */
#include "red_lut.h"

#include <stdlib.h>
#include <string.h>

/* Active model, resolved once from VBRECOMP_SCREEN. -1 = not yet resolved. */
static int s_kind = -1;

/* table[bv] = packed ARGB8888 for brightness scalar bv (0..255). */
static uint32_t s_table[256];
static int s_table_built;

bool vb_screen_kind_from_name(const char* name, VbScreenKind* out) {
  if (!name || !out) return false;
  if (strcmp(name, "raw") == 0)      { *out = VB_SCREEN_RAW;      return true; }
  if (strcmp(name, "led") == 0)      { *out = VB_SCREEN_LED;      return true; }
  if (strcmp(name, "led_warm") == 0) { *out = VB_SCREEN_LED_WARM; return true; }
  return false;
}

VbScreenKind vb_red_lut_active_kind(void) {
  if (s_kind < 0) {
    VbScreenKind k = VB_SCREEN_RAW;  /* default OFF = passthrough */
    const char* env = getenv("VBRECOMP_SCREEN");
    if (env && *env) {
      VbScreenKind parsed;
      if (vb_screen_kind_from_name(env, &parsed)) k = parsed;
      /* Unrecognized token: stay on RAW (byte-identical) rather than guess. */
    }
    s_kind = (int)k;
  }
  return (VbScreenKind)s_kind;
}

bool vb_red_lut_is_passthrough(void) {
  return vb_red_lut_active_kind() == VB_SCREEN_RAW;
}

static uint8_t clamp8(int v) {
  if (v < 0) return 0;
  if (v > 255) return 255;
  return (uint8_t)v;
}

static void build_table(void) {
  VbScreenKind kind = vb_red_lut_active_kind();
  for (int bv = 0; bv < 256; ++bv) {
    uint32_t pixel;
    switch (kind) {
      case VB_SCREEN_RAW:
      default:
        /* Exact reproduction of the canon renderer: bv is already the
         * gamma-corrected red value; emit it on the red channel, G=B=0.
         * This makes default output byte-identical to the pre-LUT path. */
        pixel = 0xFF000000u | ((uint32_t)clamp8(bv) << 16);
        break;
      case VB_SCREEN_LED: {
        /* Pure red-LED primary. bv is the same gamma-corrected red value;
         * no spill. Currently identical channels to RAW — kept as a distinct
         * mode so a measured red-LED transfer curve can replace the identity
         * here without disturbing the RAW byte-identity contract. */
        uint8_t r = clamp8(bv);
        pixel = 0xFF000000u | ((uint32_t)r << 16);
        break;
      }
      case VB_SCREEN_LED_WARM: {
        /* Small warm phosphor spill: high red intensities leak a little into
         * the green channel so the display reads as the real unit's slightly
         * orange-red at peak brightness rather than a pure-primary red. The
         * spill is intensity-weighted (none near black, ~10% of red at peak)
         * and is a presentation choice, NOT a measured device curve — it is
         * opt-in and never affects the verified raw stream. */
        uint8_t r = clamp8(bv);
        int g = (r * r) / (255 * 10);  /* quadratic, ~10% of full red at peak */
        pixel = 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)clamp8(g) << 8);
        break;
      }
    }
    s_table[bv] = pixel;
  }
  s_table_built = 1;
}

uint32_t vb_red_lut_map(int bv) {
  if (bv < 0) bv = 0;
  if (bv > 255) bv = 255;
  if (!s_table_built) build_table();
  return s_table[bv];
}
