/* vsu_shadow.c — see vsu_shadow.h.
 *
 * Verifier ported from JRickey/gba-recomp via the gbarecomp/snesrecomp ports,
 * © Jrickey, MIT OR Apache-2.0. VSU float re-render original to this project.
 */
#include "vsu_shadow.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_shadow.h"

static ShadowVerifier s_verifier;
static bool           s_verifier_inited;
static int            s_enabled = -1;   /* -1 = unresolved */
static bool           s_was_substituting; /* for proven→paused DEGRADED edge */

/* Normalize a verifier domain: the verifier compares envelope STRUCTURE and a
 * level RATIO, so any consistent scale works. We feed both sides the raw S16
 * numeric value (canon's pre-clip mix and the shadow's float mix); the ratio
 * gate then catches a wrong overall loudness while the correlation catches a
 * wrong structure. */

void vb_vsu_shadow_reset(void) {
  shadow_verifier_init(&s_verifier);
  s_verifier_inited = true;
  s_was_substituting = false;
  /* leave s_enabled cached; env doesn't change within a process */
}

bool vb_vsu_shadow_enabled(void) {
  if (s_enabled < 0) {
    s_enabled = 0;  /* default OFF */
    const char* env = getenv("VBRECOMP_AUDIO_SHADOW");
    if (env && (strcmp(env, "1") == 0 || strcmp(env, "on") == 0 ||
                strcmp(env, "true") == 0)) {
      s_enabled = 1;
    }
  }
  return s_enabled != 0;
}

static int16_t clamp_s16(float v) {
  if (v > 32767.0f) return 32767;
  if (v < -32768.0f) return -32768;
  return (int16_t)v;
}

bool vb_vsu_shadow_substitute(float canon_l, float canon_r,
                              float shadow_l, float shadow_r,
                              int16_t* out_l, int16_t* out_r) {
  if (!vb_vsu_shadow_enabled()) {
    /* Default OFF: never touch the output, never even run the verifier, so
     * the emitted bytes (and any frame hash) are byte-identical to canon. */
    return false;
  }
  if (!s_verifier_inited) vb_vsu_shadow_reset();

  /* Apply the verifier's calibrated gain to the shadow in BOTH the check copy
   * and the substituted output, exactly as the engine examples do. */
  float gain = shadow_verifier_gain(&s_verifier);
  float chk_l = shadow_l * gain;
  float chk_r = shadow_r * gain;

  ShadowJudgement j =
      shadow_verifier_judge(&s_verifier, canon_l, canon_r, chk_l, chk_r);
  (void)j;

  bool proven = shadow_verifier_proven(&s_verifier);

  /* Surface a pause as a single DEGRADED line (the verifier records the
   * reason on each pause). This is the loud-revert contract. */
  if (s_verifier.reverted[0] != '\0') {
    fprintf(stderr,
            "[DEGRADED] vsu_shadow: reverted to canon VSU mix — %s\n",
            s_verifier.reverted);
    fflush(stderr);
    s_verifier.reverted[0] = '\0';
    s_was_substituting = false;
  }

  if (proven) {
    if (!s_was_substituting) {
      fprintf(stderr,
              "[vsu_shadow] proven (r=%.2f, ratio=%.2f, gain=%.2f) — "
              "substituting float re-render\n",
              (double)s_verifier.last_r, (double)s_verifier.last_ratio,
              (double)gain);
      fflush(stderr);
      s_was_substituting = true;
    }
    *out_l = clamp_s16(chk_l);
    *out_r = clamp_s16(chk_r);
    return true;
  }
  return false;
}
