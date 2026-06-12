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

/* ── Always-on status ring (queried by the TCP `audio_shadow_state` command;
 * never armed at probe time). Replaces the old stderr DEGRADED/proven log. ── */
static VbVsuShadowEvent s_events[VB_VSU_SHADOW_EVENT_RING];
static uint32_t         s_ev_head;       /* next write slot (mod ring size) */
static uint64_t         s_ev_total;      /* events ever recorded */
static uint64_t         s_ev_seq;        /* monotonic event seq */
static uint64_t         s_samples_seen;  /* output samples fed to the shadow */
static uint64_t         s_engage_count;
static uint64_t         s_degrade_count;
static float            s_last_r;
static float            s_last_ratio;
static float            s_last_gain = 1.0f;

static void shadow_push_event(int kind, float r, float ratio, float gain,
                              const char* reason) {
  VbVsuShadowEvent* e = &s_events[s_ev_head % VB_VSU_SHADOW_EVENT_RING];
  e->seq    = ++s_ev_seq;
  e->sample = s_samples_seen;
  e->kind   = kind;
  e->r      = r;
  e->ratio  = ratio;
  e->gain   = gain;
  if (reason) snprintf(e->reason, sizeof(e->reason), "%s", reason);
  else        e->reason[0] = '\0';
  s_ev_head++;
  s_ev_total++;
}

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

  /* Clear the status ring so each run reports its own history. */
  memset(s_events, 0, sizeof(s_events));
  s_ev_head = 0;
  s_ev_total = 0;
  s_ev_seq = 0;
  s_samples_seen = 0;
  s_engage_count = 0;
  s_degrade_count = 0;
  s_last_r = 0.0f;
  s_last_ratio = 0.0f;
  s_last_gain = 1.0f;
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

  s_samples_seen++;

  /* Apply the verifier's calibrated gain to the shadow in BOTH the check copy
   * and the substituted output, exactly as the engine examples do. */
  float gain = shadow_verifier_gain(&s_verifier);
  float chk_l = shadow_l * gain;
  float chk_r = shadow_r * gain;

  ShadowJudgement j =
      shadow_verifier_judge(&s_verifier, canon_l, canon_r, chk_l, chk_r);
  (void)j;

  bool proven = shadow_verifier_proven(&s_verifier);

  s_last_r = s_verifier.last_r;
  s_last_ratio = s_verifier.last_ratio;
  s_last_gain = gain;

  /* Record a pause as a single DEGRADE event (the verifier records the reason
   * on each pause). This is the loud-revert contract — now queryable over TCP
   * instead of spilled to stderr. */
  if (s_verifier.reverted[0] != '\0') {
    shadow_push_event(VB_VSU_SHADOW_EV_DEGRADE,
                      s_verifier.last_r, s_verifier.last_ratio, gain,
                      s_verifier.reverted);
    s_degrade_count++;
    s_verifier.reverted[0] = '\0';
    s_was_substituting = false;
  }

  if (proven) {
    if (!s_was_substituting) {
      shadow_push_event(VB_VSU_SHADOW_EV_ENGAGE,
                        s_verifier.last_r, s_verifier.last_ratio, gain, NULL);
      s_engage_count++;
      s_was_substituting = true;
    }
    *out_l = clamp_s16(chk_l);
    *out_r = clamp_s16(chk_r);
    return true;
  }
  return false;
}

void vb_vsu_shadow_get_status(VbVsuShadowStatus* out) {
  if (!out) return;
  memset(out, 0, sizeof(*out));
  out->enabled       = vb_vsu_shadow_enabled();
  out->substituting  = s_was_substituting;
  out->last_r        = s_last_r;
  out->last_ratio    = s_last_ratio;
  out->gain          = s_last_gain;
  out->samples_seen  = s_samples_seen;
  out->engage_count  = s_engage_count;
  out->degrade_count = s_degrade_count;

  uint32_t count = (s_ev_total < VB_VSU_SHADOW_EVENT_RING)
                       ? (uint32_t)s_ev_total
                       : VB_VSU_SHADOW_EVENT_RING;
  for (uint32_t i = 0; i < count; ++i) {
    /* Oldest-first: the ring holds the `count` most recent events ending at
     * s_ev_head-1. */
    uint32_t idx = (s_ev_head - count + i) % VB_VSU_SHADOW_EVENT_RING;
    out->events[i] = s_events[idx];
  }
  out->n_events = count;
}
