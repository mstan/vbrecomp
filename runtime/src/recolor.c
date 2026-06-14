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
 * Horizontal regions (columns). A world rule may instead split the world's
 * on-screen bounding box HORIZONTALLY before applying vertical bands, giving a
 * 2-D grid of regions. This colors a horizontal row of distinct sprites that
 * the VIP draws under one world index (e.g. a character-roster strip):
 *   { "world": 30, "label": "roster", "cols": [
 *       { "hx": 64,  "ramp": [...mario...] },        // rel-x 0..64
 *       { "hx": 128, "bands": [ {hi,ramp}, ... ] },  // rel-x 64..128, then banded
 *       { "hx": 256, "ramp": [...] } ] }             // cols cover rel-x 0..256
 * Each column carries either a flat "ramp" or its own vertical "bands", exactly
 * like a world rule. A rule with no "cols" is one full-width column (hx=256) —
 * so the flat and banded forms above are unchanged.
 *
 * A "detect" may also carry an optional ram predicate that gates the scene on
 * a game RAM byte: "detect": { "all": [22], "ram": { "addr": "0x0500203A",
 * "eq": 1 } } matches only when that byte equals 1. This lets one on-screen
 * layout (e.g. the near tennis player, always world 22) resolve to different
 * rules per selected character. The address is game-specific and lives only in
 * the pack — never in this generic runtime.
 *
 * Detection: each frame the runtime builds a bitmask of the world indices
 * present on screen and calls vb_recolor_select_scene(). The first scene whose
 * "detect" matches ((mask & all)==all && (mask & none)==0, and the ram byte if
 * present) becomes current;
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
#include "memory.h"   /* vb_memory_dump — read a game RAM byte for ram predicates */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_SCENES 24
#define MAX_RULES  64   /* per scene */
#define MAX_BANDS  8
#define MAX_COLS   12   /* horizontal regions per rule (e.g. a roster strip) */

typedef struct { int hi; uint32_t ramp[4]; } Band;        /* hi = rel-y upper bound 0..256 */
typedef struct { int hx; int nbands; Band bands[MAX_BANDS]; } Col; /* hx = rel-x upper bound 0..256 */
typedef struct { int world; int ncols; Col cols[MAX_COLS]; } Rule;
typedef struct {
    char     name[24];
    uint32_t detect_all;   /* every one of these world indices must be present */
    uint32_t detect_none;  /* none of these world indices may be present */
    int      has_ram;      /* optional: also require a game RAM byte to equal a value */
    uint32_t ram_addr;
    int      ram_eq;
    int      nrules;
    Rule     rules[MAX_RULES];
} Scene;

static int    s_active = -1;
static Scene  s_scenes[MAX_SCENES];
static int    s_nscenes = 0;
static int    s_total_rules = 0;
static int    s_cur = -1;       /* scene chosen by the last select_scene */
static int    s_loaded = 0;

/* Hysteresis: the VB redraws sprites on alternating frames, so a world (e.g.
 * the near tennis player, world 22) drops out of ~10% of frames even while the
 * scene is visually unchanged. Without smoothing those frames match no scene
 * and render full faithful red — a harsh flicker. So when a frame matches
 * nothing, hold the previously-selected scene for up to HOLD_MISS consecutive
 * misses (≈ a few frames) before reverting to faithful. A real screen change
 * misses for far longer than HOLD_MISS, so it still reverts. */
#define HOLD_MISS 16
static int    s_miss = 0;       /* consecutive no-match select calls */

/* Always-on decision ring: every vb_recolor_select_scene call records the
 * active-world mask it saw and the scene it chose (-1 = none -> faithful frame).
 * Probes QUERY this for the window of interest instead of arming a trace. */
#define TRACE_N 512
typedef struct { uint32_t seq; uint32_t mask; int16_t scene; } TraceEnt;
static TraceEnt s_trace[TRACE_N];
static uint32_t s_trace_head = 0;   /* next write slot */
static uint32_t s_trace_seq  = 0;   /* monotonic call counter */

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

/* Parse the (possibly 0x-prefixed) hex/decimal integer following key_pos's ':'.
 * The value may be a bare number or a quoted "0x...." string. */
static uint32_t parse_uint_after(const char* key_pos, const char* end) {
    const char* c = strchr(key_pos, ':');
    if (!c || c >= end) return 0;
    c++;
    while (c < end && (*c == ' ' || *c == '\t' || *c == '"')) c++;
    return (uint32_t)strtoul(c, NULL, 0);   /* base 0 -> honors 0x prefix */
}

/* Bounded strstr: find `needle` in [hay, end). */
static const char* find_in(const char* hay, const char* end, const char* needle) {
    const char* p = strstr(hay, needle);
    return (p && p < end) ? p : NULL;
}

/* Parse all "ramp" arrays in [start, end) into col as vertical bands. Each
 * band's upper bound is the nearest preceding "hi" in [start, end) (256 if
 * none -> a single flat band). The last band is stretched to 256 so every
 * rel-y is covered. */
static void parse_bands(const char* start, const char* end, Col* col) {
    col->nbands = 0;
    const char* r = find_in(start, end, "\"ramp\"");
    while (r && col->nbands < MAX_BANDS) {
        int hi = 256;
        const char* hk = NULL;
        for (const char* s = find_in(start, r, "\"hi\""); s;
             s = find_in(s + 1, r, "\"hi\"")) hk = s;
        if (hk) hi = int_after(hk);
        uint32_t ramp[4];
        if (parse_ramp(r, end, ramp)) {
            Band* b = &col->bands[col->nbands++];
            b->hi = hi;
            for (int i = 0; i < 4; ++i) b->ramp[i] = ramp[i];
        }
        r = find_in(r + 1, end, "\"ramp\"");
    }
    if (col->nbands > 0 && col->bands[col->nbands - 1].hi < 256)
        col->bands[col->nbands - 1].hi = 256;
}

/* Parse all "world" rule objects in [start, end) into scene. */
static void parse_rules(const char* start, const char* end, Scene* sc) {
    const char* w = find_in(start, end, "\"world\"");
    while (w && sc->nrules < MAX_RULES) {
        const char* wnext = find_in(w + 1, end, "\"world\"");
        const char* span_end = wnext ? wnext : end;

        Rule* rl = &sc->rules[sc->nrules];
        rl->world = int_after(w);
        rl->ncols = 0;

        const char* cols = find_in(w, span_end, "\"cols\"");
        if (cols) {
            /* Horizontal regions: each "hx" starts a column whose span runs to
             * the next "hx" (or the world-span end). Within a column the
             * "ramp"/"hi" keys are vertical bands, exactly as in the
             * single-column case. ("hx" never matches "hi"/"world".) */
            const char* hx = find_in(cols, span_end, "\"hx\"");
            while (hx && rl->ncols < MAX_COLS) {
                const char* hxnext = find_in(hx + 1, span_end, "\"hx\"");
                const char* col_end = hxnext ? hxnext : span_end;
                Col* col = &rl->cols[rl->ncols];
                col->hx = int_after(hx);
                parse_bands(hx, col_end, col);
                if (col->nbands > 0) rl->ncols++;
                hx = hxnext;
            }
            /* ensure the last column reaches the right edge so all rel-x is
             * covered */
            if (rl->ncols > 0 && rl->cols[rl->ncols - 1].hx < 256)
                rl->cols[rl->ncols - 1].hx = 256;
        } else {
            /* One full-width column: a flat "ramp" or vertical "bands". */
            Col* col = &rl->cols[0];
            col->hx = 256;
            parse_bands(w, span_end, col);
            if (col->nbands > 0) rl->ncols = 1;
        }

        if (rl->ncols > 0) {
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
            sc->has_ram = 0; sc->ram_addr = 0; sc->ram_eq = 0;
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
                /* optional ram predicate: {"ram":{"addr":"0x...","eq":N}} */
                const char* rm = find_in(det, det_end, "\"ram\"");
                if (rm) {
                    const char* ad = find_in(rm, det_end, "\"addr\"");
                    const char* eq = find_in(rm, det_end, "\"eq\"");
                    if (ad && eq) {
                        sc->ram_addr = parse_uint_after(ad, det_end);
                        sc->ram_eq   = (int)parse_uint_after(eq, det_end);
                        sc->has_ram  = 1;
                    }
                }
            }
            parse_rules(n, span_end, sc);

            if (sc->nrules > 0) s_nscenes++;
            n = nnext;
        }
    } else {
        /* Legacy flat form: a single always-matching scene. */
        Scene* sc = &s_scenes[0];
        sc->detect_all = sc->detect_none = 0;
        sc->has_ram = 0; sc->ram_addr = 0; sc->ram_eq = 0;
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
    s_miss = 0;
    load_pack();
}

void vb_recolor_shutdown(void) {
    s_nscenes = 0; s_total_rules = 0; s_cur = -1; s_miss = 0; s_loaded = 0; s_active = -1;
}

void vb_recolor_reload(void) {
    s_nscenes = 0;
    s_total_rules = 0;
    s_cur = -1;
    s_miss = 0;
    load_pack();
    /* Re-resolve active state so a pack authored live (empty -> non-empty)
     * turns recolor on without a restart. */
    s_active = (s_total_rules > 0) ? 1 : 0;
}

int vb_recolor_entry_count(void) { return s_total_rules; }
int vb_recolor_scene_count(void) { return s_nscenes; }

static void trace_record(uint32_t mask, int scene) {
    TraceEnt* e = &s_trace[s_trace_head];
    e->seq = s_trace_seq++;
    e->mask = mask;
    e->scene = (int16_t)scene;
    s_trace_head = (s_trace_head + 1) % TRACE_N;
}

int vb_recolor_select_scene(uint32_t mask) {
    int matched = -1;
    for (int i = 0; i < s_nscenes; ++i) {
        const Scene* sc = &s_scenes[i];
        if ((mask & sc->detect_all) != sc->detect_all ||
            (mask & sc->detect_none) != 0)
            continue;
        if (sc->has_ram) {
            /* Game-state predicate: the address lives in the (per-game) pack,
             * never in this generic runtime. WRAM is always mapped, so this
             * read is safe and only runs on the opt-in recolored path. */
            uint8_t b = 0;
            vb_memory_dump(sc->ram_addr, &b, 1);
            if ((int)b != sc->ram_eq) continue;
        }
        matched = i;
        break;
    }

    int chosen;
    if (matched >= 0) {
        chosen = matched;       /* positive match always wins immediately */
        s_miss = 0;
    } else if (s_cur >= 0 && s_miss < HOLD_MISS) {
        chosen = s_cur;         /* transient dropout: hold the last scene */
        s_miss++;
    } else {
        chosen = -1;            /* sustained miss: revert to faithful */
        s_miss++;
    }

    s_cur = chosen;
    trace_record(mask, chosen);
    return chosen;
}

/* Decision-ring introspection (i=0 oldest available .. len-1 newest). */
int vb_recolor_trace_len(void) {
    return (s_trace_seq < TRACE_N) ? (int)s_trace_seq : TRACE_N;
}
int vb_recolor_trace_get(int i, uint32_t* seq, uint32_t* mask, int* scene) {
    int len = vb_recolor_trace_len();
    if (i < 0 || i >= len) return 0;
    /* oldest entry is head-len (mod N) when full, else slot 0 */
    uint32_t base = (s_trace_seq < TRACE_N) ? 0 : s_trace_head;
    uint32_t idx = (base + (uint32_t)i) % TRACE_N;
    if (seq)   *seq = s_trace[idx].seq;
    if (mask)  *mask = s_trace[idx].mask;
    if (scene) *scene = s_trace[idx].scene;
    return 1;
}

const char* vb_recolor_scene_name(int idx) {
    if (idx < 0 || idx >= s_nscenes) return "";
    return s_scenes[idx].name;
}

const char* vb_recolor_current_scene(void) {
    if (s_cur < 0 || s_cur >= s_nscenes) return "";
    return s_scenes[s_cur].name;
}

int vb_recolor_world_pixel(int world, int relx_num, int relx_den,
                           int rely_num, int rely_den, int value,
                           uint32_t* argb_out) {
    if (s_cur < 0 || s_cur >= s_nscenes) return 0;
    const Scene* sc = &s_scenes[s_cur];
    const Rule* rl = NULL;
    for (int i = 0; i < sc->nrules; ++i)
        if (sc->rules[i].world == world) { rl = &sc->rules[i]; break; }
    if (!rl || rl->ncols == 0) return 0;

    /* Pick the horizontal column covering rel-x, then the vertical band
     * covering rel-y within it. A single-column rule (ncols==1, hx==256)
     * collapses to the pure vertical-band case. */
    int relx = (relx_den > 0) ? (relx_num * 256) / relx_den : 0;
    if (relx < 0) relx = 0;
    if (relx > 255) relx = 255;
    const Col* col = NULL;
    for (int c = 0; c < rl->ncols; ++c)
        if (relx < rl->cols[c].hx) { col = &rl->cols[c]; break; }
    if (!col || col->nbands == 0) return 0;

    int rely = (rely_den > 0) ? (rely_num * 256) / rely_den : 0;
    if (rely < 0) rely = 0;
    if (rely > 255) rely = 255;
    for (int b = 0; b < col->nbands; ++b) {
        if (rely < col->bands[b].hi) {
            *argb_out = col->bands[b].ramp[value & 3];
            return 1;
        }
    }
    return 0;
}
