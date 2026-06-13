/* recolor.c — see recolor.h. Faithful-by-default present-time recolor pack. */
#include "recolor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ENTRIES 1024
#define LUT_SIZE    8192     /* 11-bit char_no + 2-bit palette */

typedef struct {
    uint32_t hash;
    int      palette;     /* -1 = any */
    uint32_t ramp[4];     /* ARGB per brightness value 0..3 */
} Entry;

static int    s_active = -1;
static Entry  s_entries[MAX_ENTRIES];
static int    s_count = 0;
static int    s_loaded = 0;

/* per-frame LUT */
static uint8_t  s_has[LUT_SIZE];
static uint32_t s_argb[LUT_SIZE][4];

static const char* overrides_dir(void) {
    const char* env = getenv("VBRECOMP_OVERRIDES");
    return (env && *env) ? env : NULL;
}

/* Parse "#rrggbb" (or "#aarrggbb") at p; returns ARGB8888, alpha forced 0xFF
 * unless 8 hex digits given. */
static uint32_t parse_hex_color(const char* p) {
    while (*p == ' ' || *p == '\t' || *p == '"') p++;
    if (*p == '#') p++;
    char buf[9]; int n = 0;
    while (n < 8 && ((p[n] >= '0' && p[n] <= '9') ||
                     (p[n] >= 'a' && p[n] <= 'f') ||
                     (p[n] >= 'A' && p[n] <= 'F'))) { buf[n] = p[n]; n++; }
    buf[n] = 0;
    unsigned long v = strtoul(buf, NULL, 16);
    if (n <= 6) return 0xFF000000u | (uint32_t)v;   /* RRGGBB */
    return (uint32_t)v;                              /* AARRGGBB */
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

    /* Each entry has exactly one "ramp"; anchor on it, read the nearest
     * preceding "hash" and "palette" and the 4 colors after "ramp". */
    const char* p = t;
    const char* end = t + len;
    while (s_count < MAX_ENTRIES) {
        const char* ramp = strstr(p, "\"ramp\"");
        if (!ramp) break;

        /* nearest "hash" before this ramp */
        const char* hk = NULL;
        for (const char* s = strstr(t, "\"hash\""); s && s < ramp;
             s = strstr(s + 1, "\"hash\"")) hk = s;
        uint32_t hash = 0; int have_hash = 0;
        if (hk) {
            const char* c = strchr(hk, ':');
            if (c) { while (*++c == ' ' || *c == '\t' || *c == '"') {}
                     hash = (uint32_t)strtoul(c, NULL, 16); have_hash = 1; }
        }
        /* nearest "palette" before this ramp (optional) */
        int palette = -1;
        {
            const char* pk = NULL;
            for (const char* s = strstr(t, "\"palette\""); s && s < ramp;
                 s = strstr(s + 1, "\"palette\"")) pk = s;
            if (pk) { const char* c = strchr(pk, ':');
                      if (c) palette = (int)strtol(c + 1, NULL, 10); }
        }
        /* 4 hex colors after "ramp": */
        uint32_t r[4] = {0,0,0,0};
        const char* c = strchr(ramp, '[');
        int ok = (c != NULL) && have_hash;
        if (c) {
            c++;
            for (int i = 0; i < 4 && c < end; ++i) {
                const char* q = strchr(c, '#');
                if (!q || q >= end) { ok = 0; break; }
                r[i] = parse_hex_color(q);
                c = q + 1;
            }
        }
        if (ok) {
            Entry* e = &s_entries[s_count++];
            e->hash = hash; e->palette = palette;
            for (int i = 0; i < 4; ++i) e->ramp[i] = r[i];
        }
        p = ramp + 6;
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

void vb_recolor_shutdown(void) {
    s_count = 0; s_loaded = 0; s_active = -1;
}

void vb_recolor_reload(void) {
    s_count = 0;
    load_pack();
    /* keep s_active as-is so attribution/present stay enabled across a reload */
}

int vb_recolor_entry_count(void) { return s_count; }

void vb_recolor_frame_reset(void) {
    memset(s_has, 0, sizeof(s_has));
}

void vb_recolor_resolve(uint16_t id, uint32_t hash, int palette) {
    if (id >= LUT_SIZE) return;
    /* Prefer an exact palette match over a wildcard (-1) entry. */
    const Entry* best = NULL;
    for (int i = 0; i < s_count; ++i) {
        const Entry* e = &s_entries[i];
        if (e->hash != hash) continue;
        if (e->palette == palette) { best = e; break; }
        if (e->palette < 0 && !best) best = e;
    }
    if (!best) return;
    s_has[id] = 1;
    for (int v = 0; v < 4; ++v) s_argb[id][v] = best->ramp[v];
}

int vb_recolor_pixel(uint16_t id, int value, uint32_t* argb_out) {
    if (id >= LUT_SIZE || !s_has[id]) return 0;
    *argb_out = s_argb[id][value & 3];
    return 1;
}
