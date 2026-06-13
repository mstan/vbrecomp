/* asset_pack.c — see asset_pack.h.
 *
 * Faithful-by-default override loader. All state is lazily populated only
 * when VBRECOMP_OVERRIDES is set and a valid manifest is found; otherwise
 * every entry point is inert and the renderer/present path is byte-identical.
 */
#include "asset_pack.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png_read.h"

#define MAX_IMAGES   256
#define MAX_OVERLAY  2048

static int s_active = -1;          /* -1 unresolved */
static const char* s_dir = NULL;   /* $VBRECOMP_OVERRIDES */
static VbOverrideImage s_imgs[MAX_IMAGES];
static int s_img_count = 0;
static int s_loaded = 0;

static VbOverlayCmd s_overlay[2][MAX_OVERLAY];
static int s_overlay_n[2] = {0, 0};

int vb_overrides_active(void) {
    if (s_active < 0) {
        const char* env = getenv("VBRECOMP_OVERRIDES");
        s_active = (env && *env) ? 1 : 0;
        s_dir = (s_active ? env : NULL);
    }
    return s_active;
}

/* ---- tiny manifest scan (the manifest is authored by our own tools;
 * scan field-by-field rather than pull in a JSON library). ---- */

static int find_str(const char* p, const char* end, const char* key,
                    char* out, size_t max) {
    const char* k = strstr(p, key);
    if (!k || k >= end) return 0;
    const char* c = strchr(k, ':');
    if (!c || c >= end) return 0;
    c++;
    while (*c == ' ' || *c == '\t') c++;
    if (*c != '"') return 0;
    c++;
    size_t n = 0;
    while (*c && *c != '"' && n + 1 < max && c < end) out[n++] = *c++;
    out[n] = 0;
    return 1;
}

static int find_int(const char* p, const char* end, const char* key, int* out) {
    const char* k = strstr(p, key);
    if (!k || k >= end) return 0;
    const char* c = strchr(k, ':');
    if (!c || c >= end) return 0;
    *out = (int)strtol(c + 1, NULL, 10);
    return 1;
}

static void load_manifest(void) {
    char mpath[512];
    snprintf(mpath, sizeof(mpath), "%s/graphics/manifest.json", s_dir);
    FILE* f = fopen(mpath, "rb");
    if (!f) return;                      /* no manifest → faithful */
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (len <= 0 || len > (1 << 20)) { fclose(f); return; }
    char* text = (char*)malloc((size_t)len + 1);
    if (!text) { fclose(f); return; }
    if (fread(text, 1, (size_t)len, f) != (size_t)len) {
        free(text); fclose(f); return;
    }
    text[len] = 0;
    fclose(f);

    /* Each replacement object carries exactly one "image"; pair it with the
     * nearest preceding "tile_hash" and the following anchor fields. */
    const char* p = text;
    const char* fileend = text + len;
    while (s_img_count < MAX_IMAGES) {
        const char* img = strstr(p, "\"image\"");
        if (!img) break;

        /* nearest "tile_hash" before this image */
        char hashstr[32] = {0};
        const char* th = NULL;
        for (const char* scan = strstr(text, "\"tile_hash\"");
             scan && scan < img;
             scan = strstr(scan + 1, "\"tile_hash\"")) {
            th = scan;
        }
        char imgfile[256] = {0};
        int anchor_x = 0, anchor_y = 0;
        if (th) find_str(th, img + 64, "\"tile_hash\"", hashstr, sizeof(hashstr));
        find_str(img, fileend, "\"image\"", imgfile, sizeof(imgfile));
        /* anchors are optional, search a bounded window after image */
        const char* win_end = img + 256 < fileend ? img + 256 : fileend;
        find_int(img, win_end, "\"anchor_x\"", &anchor_x);
        find_int(img, win_end, "\"anchor_y\"", &anchor_y);

        if (hashstr[0] && imgfile[0]) {
            uint32_t hash = (uint32_t)strtoul(hashstr, NULL, 16);
            char ipath[512];
            snprintf(ipath, sizeof(ipath), "%s/graphics/%s", s_dir, imgfile);
            uint32_t* argb = NULL; int w = 0, h = 0;
            if (vb_read_png_rgba(ipath, &argb, &w, &h) == 0) {
                VbOverrideImage* e = &s_imgs[s_img_count++];
                e->hash = hash; e->w = w; e->h = h;
                e->anchor_x = anchor_x; e->anchor_y = anchor_y;
                e->argb = argb;
            }
            /* a missing/invalid image is simply skipped — faithful for it */
        }
        p = img + 7;
    }
    free(text);
}

void vb_overrides_init(void) {
    if (!vb_overrides_active() || s_loaded) return;
    s_loaded = 1;
    s_img_count = 0;
    load_manifest();
}

void vb_overrides_shutdown(void) {
    for (int i = 0; i < s_img_count; ++i) free(s_imgs[i].argb);
    s_img_count = 0;
    s_loaded = 0;
}

const VbOverrideImage* vb_overrides_lookup_tile(uint32_t tile_hash) {
    if (!s_active || !s_img_count) return NULL;
    for (int i = 0; i < s_img_count; ++i)
        if (s_imgs[i].hash == tile_hash) return &s_imgs[i];
    return NULL;
}

int vb_overrides_image_count(void) { return s_img_count; }

void vb_overlay_reset(int slot) {
    if (slot < 0 || slot > 1) return;
    s_overlay_n[slot] = 0;
}

void vb_overlay_add(int slot, const VbOverlayCmd* cmd) {
    if (slot < 0 || slot > 1 || !cmd) return;
    if (s_overlay_n[slot] >= MAX_OVERLAY) return;
    s_overlay[slot][s_overlay_n[slot]++] = *cmd;
}

int vb_overlay_count(int slot) {
    return (slot == 0 || slot == 1) ? s_overlay_n[slot] : 0;
}

const VbOverlayCmd* vb_overlay_cmds(int slot) {
    return (slot == 0 || slot == 1) ? s_overlay[slot] : NULL;
}

void vb_overlay_composite(uint32_t* eye_buf, int w, int h, int eye, int slot) {
    if (!eye_buf || (slot != 0 && slot != 1)) return;
    int n = s_overlay_n[slot];
    const VbOverlayCmd* cmds = s_overlay[slot];
    for (int i = 0; i < n; ++i) {
        const VbOverlayCmd* c = &cmds[i];
        if (eye == 0 ? !c->vis_l : !c->vis_r) continue;
        const VbOverrideImage* im = c->img;
        if (!im || !im->argb) continue;
        int ox = (eye == 0 ? c->x_l : c->x_r) + im->anchor_x;
        int oy = c->y + im->anchor_y;
        for (int row = 0; row < im->h; ++row) {
            int sy = c->vflip ? (im->h - 1 - row) : row;
            int dy = oy + row;
            if (dy < 0 || dy >= h) continue;
            for (int col = 0; col < im->w; ++col) {
                int sx = c->hflip ? (im->w - 1 - col) : col;
                int dx = ox + col;
                if (dx < 0 || dx >= w) continue;
                uint32_t s = im->argb[sy * im->w + sx];
                uint32_t a = s >> 24;
                if (a == 0) continue;
                uint32_t* d = &eye_buf[dy * w + dx];
                if (a == 0xFF) {
                    *d = 0xFF000000u | (s & 0x00FFFFFFu);
                    continue;
                }
                uint32_t dr = (*d >> 16) & 0xFF, dg = (*d >> 8) & 0xFF, db = *d & 0xFF;
                uint32_t sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
                uint32_t r = (sr * a + dr * (255 - a)) / 255;
                uint32_t g = (sg * a + dg * (255 - a)) / 255;
                uint32_t b = (sb * a + db * (255 - a)) / 255;
                *d = 0xFF000000u | (r << 16) | (g << 8) | b;
            }
        }
    }
}
