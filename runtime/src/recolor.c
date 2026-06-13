/* recolor.c — see recolor.h. Faithful-by-default scene-aware world recolor.
 *
 * Pack format (overrides/recolor/palette.json):
 *   { "scenes": [
 *       { "name": "match",
 *         "detect": { "all": [22], "none": [29] },   // active-world signature
 *         "worlds": [
 *           { "world": 22, "label": "mario", "bands": [
 *               { "hi": 96,  "ramp": ["#000","#7a0000","#c81010","#ff3030"] },
 *               { "hi": 256, "ramp": [...] } ] },     // bands cover rel-y 0..256
 *           { "world": 28, "label": "court", "ramp": [ ...4 colors... ] } ]  // flat
 *       },
 *       { "name": "title", "detect": { "all": [25, 26] }, "worlds": [...] }
 *   ] }
 *
 * Detection: each frame the runtime builds a bitmask of the world indices
 * present on screen and calls vb_recolor_select_scene(). The first scene whose
 * "detect" matches ((mask & all)==all && (mask & none)==0) becomes current;
 * its world rules drive vb_recolor_world_pixel. A scene with an empty/absent
 * "detect" matches always (order it last as a catch-all). If no scene matches,
 * the frame renders faithfully.
 *
 * Legacy flat form ({"worlds":[...]}) with no "scenes" is accepted and loaded
 * as a single always-matching scene.
 *
 * A world rule with a single "ramp" (no "bands") is flat (one band, hi=256).
 */
#include "recolor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SCENES 16
#define MAX_RULES  64   /* per scene */
#define MAX_BANDS  8

typedef struct { int hi; uint32_t ramp[4]; } Band;        /* hi = rel-y upper bound 0..256 */
typedef struct { int world; int nbands; Band bands[MAX_BANDS]; } Rule;
typedef struct {
    char     name[24];
    uint32_t detect_all;   /* every one of these world indices must be present */
    uint32_t detect_none;  /* none of these world indices may be present */
    int      nrules;
    Rule     rules[MAX_RULES];
} Scene;

static int    s_active = -1;
static Scene  s_scenes[MAX_SCENES];
static int    s_nscenes = 0;
static int    s_total_rules = 0;
static int    s_cur = -1;       /* scene chosen by the last select_scene */
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
    if (!c || c >= end) return 0;
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

/* OR (1<<n) for each int n in the JSON array following key_pos's ':'. */
static uint32_t parse_int_mask(const char* key_pos, const char* end) {
    const char* c = strchr(key_pos, '[');
    if (!c || c >= end) return 0;
    c++;
    uint32_t mask = 0;
    while (c < end && *c != ']') {
        while (c < end && (*c == ' ' || *c == ',' || *c == '\t' ||
                           *c == '\n' || *c == '\r')) c++;
        if (c >= end || *c == ']') break;
        if (*c >= '0' && *c <= '9') {
            int n = (int)strtol(c, (char**)&c, 10);
            if (n >= 0 && n < 32) mask |= (1u << n);
        } else {
            c++;
        }
    }
    return mask;
}

/* Bounded strstr: find `needle` in [hay, end). */
static const char* find_in(const char* hay, const char* end, const char* needle) {
    const char* p = strstr(hay, needle);
    return (p && p < end) ? p : NULL;
}

/* Parse all "world" rule objects in [start, end) into scene. */
static void parse_rules(const char* start, const char* end, Scene* sc) {
    const char* w = find_in(start, end, "\"world\"");
    while (w && sc->nrules < MAX_RULES) {
        const char* wnext = find_in(w + 1, end, "\"world\"");
        const char* span_end = wnext ? wnext : end;

        Rule* rl = &sc->rules[sc->nrules];
        rl->world  = int_after(w);
        rl->nbands = 0;

        /* Each "ramp" in this span is a band; its upper bound is the nearest
         * preceding "hi" in the span (or 256 if none -> flat). */
        const char* r = find_in(w, span_end, "\"ramp\"");
        while (r && rl->nbands < MAX_BANDS) {
            int hi = 256;
            const char* hk = NULL;
            for (const char* s = find_in(w, r, "\"hi\""); s;
                 s = find_in(s + 1, r, "\"hi\"")) hk = s;
            if (hk) hi = int_after(hk);
            uint32_t ramp[4];
            if (parse_ramp(r, span_end, ramp)) {
                Band* b = &rl->bands[rl->nbands++];
                b->hi = hi;
                for (int i = 0; i < 4; ++i) b->ramp[i] = ramp[i];
            }
            r = find_in(r + 1, span_end, "\"ramp\"");
        }
        if (rl->nbands > 0) {
            /* ensure the last band reaches the bottom so all rel-y is covered */
            if (rl->bands[rl->nbands - 1].hi < 256)
                rl->bands[rl->nbands - 1].hi = 256;
            sc->nrules++;
            s_total_rules++;
        }
        w = wnext;
    }
}

static void copy_scene_name(Scene* sc, const char* name_key, const char* end) {
    sc->name[0] = 0;
    const char* c = strchr(name_key, ':');
    if (!c || c >= end) return;
    const char* q = strchr(c, '"');
    if (!q || q >= end) return;
    q++;
    int i = 0;
    while (q < end && *q != '"' && i < (int)sizeof(sc->name) - 1)
        sc->name[i++] = *q++;
    sc->name[i] = 0;
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

    if (find_in(t, end, "\"scenes\"")) {
        /* Scene-aware form: one scene per "name" key; its span runs to the
         * next "name" (or end). "name" is a scene-only key — rule objects use
         * "label"/"world", never "name" — so it cleanly delimits scenes. */
        const char* n = find_in(t, end, "\"name\"");
        while (n && s_nscenes < MAX_SCENES) {
            const char* nnext = find_in(n + 1, end, "\"name\"");
            const char* span_end = nnext ? nnext : end;

            Scene* sc = &s_scenes[s_nscenes];
            sc->detect_all = sc->detect_none = 0;
            sc->nrules = 0;
            copy_scene_name(sc, n, span_end);

            const char* det = find_in(n, span_end, "\"detect\"");
            if (det) {
                /* "worlds" begins the rule list; keep detect parsing before it */
                const char* wlist = find_in(det, span_end, "\"worlds\"");
                const char* det_end = wlist ? wlist : span_end;
                const char* a = find_in(det, det_end, "\"all\"");
                const char* o = find_in(det, det_end, "\"none\"");
                if (a) sc->detect_all  = parse_int_mask(a, det_end);
                if (o) sc->detect_none = parse_int_mask(o, det_end);
            }
            parse_rules(n, span_end, sc);

            if (sc->nrules > 0) s_nscenes++;
            n = nnext;
        }
    } else {
        /* Legacy flat form: a single always-matching scene. */
        Scene* sc = &s_scenes[0];
        sc->detect_all = sc->detect_none = 0;
        sc->nrules = 0;
        snprintf(sc->name, sizeof(sc->name), "default");
        parse_rules(t, end, sc);
        if (sc->nrules > 0) s_nscenes = 1;
    }
    free(t);
}

int vb_recolor_active(void) {
    if (s_active < 0) {
        vb_recolor_init();
        s_active = (s_total_rules > 0) ? 1 : 0;
    }
    return s_active;
}

void vb_recolor_init(void) {
    if (s_loaded) return;
    s_loaded = 1;
    s_nscenes = 0;
    s_total_rules = 0;
    s_cur = -1;
    load_pack();
}

void vb_recolor_shutdown(void) {
    s_nscenes = 0; s_total_rules = 0; s_cur = -1; s_loaded = 0; s_active = -1;
}

void vb_recolor_reload(void) {
    s_nscenes = 0;
    s_total_rules = 0;
    s_cur = -1;
    load_pack();
    /* Re-resolve active state so a pack authored live (empty -> non-empty)
     * turns recolor on without a restart. */
    s_active = (s_total_rules > 0) ? 1 : 0;
}

int vb_recolor_entry_count(void) { return s_total_rules; }
int vb_recolor_scene_count(void) { return s_nscenes; }

int vb_recolor_select_scene(uint32_t mask) {
    for (int i = 0; i < s_nscenes; ++i) {
        const Scene* sc = &s_scenes[i];
        if ((mask & sc->detect_all) == sc->detect_all &&
            (mask & sc->detect_none) == 0) {
            s_cur = i;
            return i;
        }
    }
    s_cur = -1;
    return -1;
}

const char* vb_recolor_current_scene(void) {
    if (s_cur < 0 || s_cur >= s_nscenes) return "";
    return s_scenes[s_cur].name;
}

int vb_recolor_world_pixel(int world, int rel_num, int rel_den, int value,
                           uint32_t* argb_out) {
    if (s_cur < 0 || s_cur >= s_nscenes) return 0;
    const Scene* sc = &s_scenes[s_cur];
    const Rule* rl = NULL;
    for (int i = 0; i < sc->nrules; ++i)
        if (sc->rules[i].world == world) { rl = &sc->rules[i]; break; }
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
