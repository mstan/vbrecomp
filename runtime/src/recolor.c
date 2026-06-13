/* recolor.c — see recolor.h. Faithful-by-default world-based recolor pack.
 *
 * Pack format (overrides/recolor/palette.json):
 *   { "worlds": [
 *       { "world": 24, "label": "mario", "bands": [
 *           { "hi": 96,  "ramp": ["#000","#7a0000","#c81010","#ff3030"] },
 *           { "hi": 160, "ramp": [...] },   // bands cover rel-y in 0..256
 *           { "hi": 256, "ramp": [...] } ] },
 *       { "world": 30, "label": "court", "ramp": [ ...4 colors... ] }  // flat
 *   ] }
 * A world rule with a single "ramp" (no "bands") is flat (one band, hi=256).
 */
#include "recolor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_RULES 64
#define MAX_BANDS 8

typedef struct { int hi; uint32_t ramp[4]; } Band;        /* hi = rel-y upper bound 0..256 */
typedef struct { int world; int nbands; Band bands[MAX_BANDS]; } Rule;

static int    s_active = -1;
static Rule   s_rules[MAX_RULES];
static int    s_count = 0;
static int    s_loaded = 0;

static const char* overrides_dir(void) {
    const char* env = getenv("VBRECOMP_OVERRIDES");
    return (env && *env) ? env : NULL;
}

static uint32_t parse_hex_color(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    if (*p == '#') p++;
    char buf[9]; int n = 0;
    while (n < 8 && ((p[n] >= '0' && p[n] <= '9') ||
                     (p[n] >= 'a' && p[n] <= 'f') ||
                     (p[n] >= 'A' && p[n] <= 'F'))) { buf[n] = p[n]; n++; }
    buf[n] = 0;
    unsigned long v = strtoul(buf, NULL, 16);
    if (n <= 6) return 0xFF000000u | (uint32_t)v;
    return (uint32_t)v;
}

/* Read 4 "#rrggbb" colors from the array starting at `arr` ('[' expected). */
static int parse_ramp(const char* arr, const char* end, uint32_t out[4]) {
    const char* c = strchr(arr, '[');
    if (!c) return 0;
    c++;
    for (int i = 0; i < 4; ++i) {
        const char* q = strchr(c, '#');
        if (!q || q >= end) return 0;
        out[i] = parse_hex_color(q);
        c = q + 1;
    }
    return 1;
}

static int int_after(const char* key_pos) {
    const char* c = strchr(key_pos, ':');
    return c ? (int)strtol(c + 1, NULL, 10) : 0;
}

static void load_pack(void) {
    const char* dir = overrides_dir();
    if (!dir) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/recolor/palette.json", dir);
    FILE* f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long len = ftell(f); fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (4 << 20)) { fclose(f); return; }
    char* t = (char*)malloc((size_t)len + 1);
    if (!t) { fclose(f); return; }
    if (fread(t, 1, (size_t)len, f) != (size_t)len) { free(t); fclose(f); return; }
    t[len] = 0; fclose(f);
    const char* end = t + len;

    /* One rule per "world" key. The rule spans [this "world", next "world"). */
    const char* w = strstr(t, "\"world\"");
    while (w && s_count < MAX_RULES) {
        const char* wnext = strstr(w + 1, "\"world\"");
        const char* span_end = wnext ? wnext : end;

        Rule* rl = &s_rules[s_count];
        rl->world = int_after(w);
        rl->nbands = 0;

        /* Each "ramp" in this span is a band; its upper bound is the nearest
         * preceding "hi" in the span (or 256 if none → flat). */
        const char* r = strstr(w, "\"ramp\"");
        while (r && r < span_end && rl->nbands < MAX_BANDS) {
            int hi = 256;
            const char* hk = NULL;
            for (const char* s = strstr(w, "\"hi\""); s && s < r;
                 s = strstr(s + 1, "\"hi\"")) { if (s < span_end) hk = s; }
            if (hk) hi = int_after(hk);
            uint32_t ramp[4];
            if (parse_ramp(r, span_end, ramp)) {
                Band* b = &rl->bands[rl->nbands++];
                b->hi = hi;
                for (int i = 0; i < 4; ++i) b->ramp[i] = ramp[i];
            }
            r = strstr(r + 1, "\"ramp\"");
        }
        if (rl->nbands > 0) {
            /* ensure the last band reaches the bottom so all rel-y is covered */
            if (rl->bands[rl->nbands - 1].hi < 256)
                rl->bands[rl->nbands - 1].hi = 256;
            s_count++;
        }
        w = wnext;
    }
    free(t);
}

int vb_recolor_active(void) {
    if (s_active < 0) {
        vb_recolor_init();
        s_active = (s_count > 0) ? 1 : 0;
    }
    return s_active;
}

void vb_recolor_init(void) {
    if (s_loaded) return;
    s_loaded = 1;
    s_count = 0;
    load_pack();
}

void vb_recolor_shutdown(void) { s_count = 0; s_loaded = 0; s_active = -1; }

void vb_recolor_reload(void) {
    s_count = 0;
    load_pack();
    /* Re-resolve active state so a pack authored live (empty -> non-empty)
     * turns recolor on without a restart. */
    s_active = (s_count > 0) ? 1 : 0;
}

int vb_recolor_entry_count(void) { return s_count; }

int vb_recolor_world_pixel(int world, int rel_num, int rel_den, int value,
                           uint32_t* argb_out) {
    const Rule* rl = NULL;
    for (int i = 0; i < s_count; ++i)
        if (s_rules[i].world == world) { rl = &s_rules[i]; break; }
    if (!rl || rl->nbands == 0) return 0;
    int rel = (rel_den > 0) ? (rel_num * 256) / rel_den : 0;
    if (rel < 0) rel = 0;
    if (rel > 255) rel = 255;
    for (int b = 0; b < rl->nbands; ++b) {
        if (rel < rl->bands[b].hi) {
            *argb_out = rl->bands[b].ramp[value & 3];
            return 1;
        }
    }
    return 0;
}
