/* Read-only presentation replay of VIP drawing inputs. Raster addressing and
 * clipping follow vip.c / Beetle vip_draw.inc. Guest drawing remains in vip.c. */
#include "viewport.h"
#include "vip.h"
#include <string.h>

typedef struct ViewBlock {
    uint16_t dram[65536], chars[16384], spt[4], suppress[1024];
    uint8_t gplt[4][4], jplt[4][4], backdrop, valid;
    uint8_t extra[VB_VIEWPORT_MAX_EXTRA];
} ViewBlock;
static ViewBlock blocks[2][28];
static int tracking, adaptive, window_width = 1280, window_height = 720;
static unsigned aspect_n = 16, aspect_d = 9;
static char identifier[96];
static VbViewportClassify classify_scene;
static VbViewportArrange arrange_scene;
static void* policy_context;
static unsigned extra_size;
static VbViewportCaptureExtra capture_extra;

static int sx(unsigned v, unsigned bits) {
    unsigned mask = (1u << bits) - 1, sign = 1u << (bits - 1);
    return (int)((v & mask) ^ sign) - (int)sign;
}
static VbViewportScene scene_of(const ViewBlock* block) {
    VbViewportScene scene = {block->dram + 0x1d800 / 2,
                            block->dram + 0x1e000 / 2, block->chars, block->dram,
                            block->spt, block->extra, extra_size};
    return scene;
}
void vb_viewport_track(void) { tracking = 1; }
int vb_viewport_track_extra(unsigned size, VbViewportCaptureExtra capture) {
    if (!size || size > VB_VIEWPORT_MAX_EXTRA || !capture || capture_extra) return 0;
    extra_size = size; capture_extra = capture; return 1;
}
void vb_viewport_clear_frames(void) {
    for (int slot = 0; slot < 2; ++slot)
        for (int row = 0; row < 28; ++row) blocks[slot][row].valid = 0;
}
void vb_viewport_reset(void) {
    identifier[0] = 0; classify_scene = 0; arrange_scene = 0; policy_context = 0;
    adaptive = 0; aspect_n = 16; aspect_d = 9;
}
int vb_viewport_register(const char* id, unsigned n, unsigned d, int fit,
                         VbViewportClassify classify, VbViewportArrange arrange,
                         void* context) {
    if (!tracking || !id || !*id || strlen(id) >= sizeof(identifier) || identifier[0]
        || !n || !d || n > 4096 || d > 4096 || !classify || !arrange) return 0;
    strcpy(identifier, id); aspect_n = n; aspect_d = d; adaptive = fit != 0;
    classify_scene = classify; arrange_scene = arrange; policy_context = context;
    return 1;
}
int vb_viewport_active(void) { return identifier[0] != 0; }
const char* vb_viewport_id(void) { return identifier; }
void vb_viewport_window(int width, int height) {
    if (width > 0 && height > 0) { window_width = width; window_height = height; }
}
int vb_viewport_width(void) {
    if (!classify_scene) return 384;
    int supported = 0, slot = vb_vip_display_fb() & 1;
    for (int row = 0; row < 28; ++row) if (!blocks[slot][row].valid) return 384;
    for (int row = 0; row < 28; ++row) {
        VbViewportScene scene = scene_of(&blocks[slot][row]);
        if (classify_scene(&scene, policy_context)) { supported = 1; break; }
    }
    if (!supported) return 384;
    double ratio = adaptive ? (double)window_width / window_height : (double)aspect_n / aspect_d;
    double desired = ratio * 224;
    if (desired <= 384) return 384;
    if (desired >= VB_VIEWPORT_MAX_WIDTH) return VB_VIEWPORT_MAX_WIDTH;
    return (int)(desired / 2 + .5) * 2; /* An integer, centered native pixel grid. */
}
void vb_viewport_capture(int slot, int row, const uint16_t* dram,
                         const uint16_t* chars, const uint16_t spt[4],
                         const uint8_t gplt[4][4], const uint8_t jplt[4][4],
                         unsigned backdrop, const uint16_t suppress[1024]) {
    if (!tracking || slot < 0 || slot > 1 || row < 0 || row >= 28) return;
    ViewBlock* b = &blocks[slot][row];
    memcpy(b->dram, dram, sizeof(b->dram)); memcpy(b->chars, chars, sizeof(b->chars));
    memcpy(b->spt, spt, sizeof(b->spt)); memcpy(b->suppress, suppress, sizeof(b->suppress));
    memcpy(b->gplt, gplt, sizeof(b->gplt)); memcpy(b->jplt, jplt, sizeof(b->jplt));
    if (capture_extra) capture_extra(b->extra, extra_size);
    b->backdrop = backdrop & 3; b->valid = 1;
}

typedef struct ViewTarget {
    const ViewBlock* block;
    const VbViewportLayout* layout;
    int width, margin, eye, world, kind, map, y;
    uint8_t* levels;
    uint16_t* worlds;
    VbSourceTexel* sources;
    uint32_t hashes[2048];
    uint8_t hashed[2048];
} ViewTarget;

static void pixel(ViewTarget* target, int x, unsigned tile, unsigned u, unsigned v,
                  unsigned source_x, unsigned source_y, unsigned palette, int object) {
    const ViewBlock* b = target->block;
    tile &= 2047;
    unsigned raw = (b->chars[tile * 8 + v] >> (u * 2)) & 3;
    if (!raw) return;
    const VbViewportWorldLayout* rule = &target->layout->worlds[target->world];
    if (target->y >= rule->top && target->y < rule->bottom)
        x += x < rule->left_end ? rule->left_shift : x >= rule->right_start
             ? rule->right_shift : rule->center_shift;
    x += target->margin;
    if (x < 0 || x >= target->width) return;
    unsigned index = target->y * target->width + x;
    target->levels[index] = object ? b->jplt[palette][raw] : b->gplt[palette][raw];
    target->worlds[index] = (uint16_t)(target->world + 1);
    if (!target->hashed[tile]) {
        const uint8_t* bytes = (const uint8_t*)&b->chars[tile * 8];
        uint32_t hash = 2166136261u;
        for (int i = 0; i < 16; ++i) hash = (hash ^ bytes[i]) * 16777619u;
        target->hashes[tile] = hash; target->hashed[tile] = 1;
    }
    target->sources[index] = (VbSourceTexel){target->hashes[tile], (uint16_t)source_x,
        (uint16_t)source_y, (uint16_t)tile, (uint8_t)u, (uint8_t)v,
        (uint8_t)target->map, (uint8_t)target->kind, (uint8_t)raw,
        (uint8_t)(target->world + 1)};
}

static void background(ViewTarget* t, const uint16_t* w, int limited) {
    const uint16_t* dram = t->block->dram;
    int eye = t->eye;
    int gx = sx((unsigned)sx(w[1], 11) + (eye ? sx(w[2], 9) : -sx(w[2], 9)), 10);
    int gy = sx(w[3], 11);
    unsigned ry = (uint16_t)(t->y - gy), height = w[8] & 1023;
    if (ry > height) return;
    unsigned width = (uint16_t)sx(w[7], 11), scx = (w[0] >> 10) & 3, scy = (w[0] >> 8) & 3;
    unsigned xs = 512u << scx, ys = 512u << scy, base = (w[0] & 15u) << 12;
    int over = (w[0] & 128) != 0;
    const VbViewportWorldLayout* layout = &t->layout->worlds[t->world];
    int x0 = gx, x1 = gx + (int)width;
    int min_x = limited ? 0 : -t->margin;
    int max_x = limited ? 383 : t->width - t->margin - 1;
    if (t->kind == 0 && layout->outside && !limited) { x0 = min_x; x1 = max_x; }
    /* Anchored HUD pixels may be inside the native window but outside a
     * translated output interval. Sample their original coordinates first. */
    if (x0 < min_x) x0 = min_x;
    if (x1 > max_x) x1 = max_x;
    if (x0 > x1) return;
    if (t->kind == 2) {
        unsigned param = ((w[9] & 0xfff0u) + 8u * ry) & 65535;
        int mx = (int16_t)dram[param], mp = (int16_t)dram[(param + 1) & 65535];
        int my = (int16_t)dram[(param + 2) & 65535];
        int dx = (int16_t)dram[(param + 3) & 65535], dy = (int16_t)dram[(param + 4) & 65535];
        int shift = mp >= 0 && eye ? mp : mp < 0 && !eye ? -mp : 0;
        uint32_t xx = (uint32_t)(mx * 64 + dx * (x0 - gx + shift));
        uint32_t yy = (uint32_t)(my * 64 + dy * (x0 - gx + shift));
        unsigned xmask = over ? 0x3ffffff : (xs << 9) - 1;
        unsigned ymask = over ? 0x3ffffff : (ys << 9) - 1;
        for (int x = x0; x <= x1; ++x) {
            xx &= xmask; yy &= ymask;
            unsigned cell = dram[w[10]];
            if (xx < (xs << 9) && yy < (ys << 9)) {
                unsigned at = base | ((xx >> 6) & ~4095u)
                    | (((yy >> 6) & ~4095u) << scx)
                    | ((xx >> 12) & 63u) | (((yy >> 12) & 63u) * 64);
                cell = dram[at & 65535];
            } else if (!dy) {
                /* Native DrawAffine's horizontal fast path rejects rows
                 * outside the source even when overplane mode is set. */
                if (yy >= (ys << 9)) return;
            }
            pixel(t, x, cell & 2047, ((xx >> 9) & 7) ^ ((cell & 0x2000) ? 7 : 0),
                  ((yy >> 9) & 7) ^ ((cell & 0x1000) ? 7 : 0), xx >> 9, yy >> 9, cell >> 14, 0);
            xx += (uint32_t)dx; yy += (uint32_t)dy;
        }
    } else {
        int source_x = (int16_t)(w[4] + (eye ? sx(w[5], 9) : -sx(w[5], 9)));
        unsigned source_y = (uint16_t)(w[6] + ry);
        if (t->kind == 1)
            source_x = (int16_t)(source_x + (int16_t)dram[((w[9] & 0xfff0u) + ((ry * 2u) | eye)) & 65535]);
        unsigned yy = source_y & (over ? 8191 : ys - 1);
        for (int x = x0; x <= x1; ++x) {
            int sample_x = source_x + x - gx, sample_y = source_y;
            if (layout->outside && (x < gx || x > gx + (int)width)
                && !layout->outside(sample_x, sample_y, &sample_x, &sample_y, layout->outside_context)) continue;
            unsigned xx = (unsigned)sample_x & (over ? 8191 : xs - 1);
            yy = (unsigned)sample_y & (over ? 8191 : ys - 1);
            unsigned cell = dram[w[10]];
            if (xx < xs && yy < ys) {
                unsigned at = base | ((xx << 3) & ~4095u) | (((yy << 3) & ~4095u) << scx)
                    | ((xx >> 3) & 63u) | (((yy >> 3) & 63u) * 64);
                cell = dram[at & 65535];
            }
            pixel(t, x, cell & 2047, (xx & 7) ^ ((cell & 0x2000) ? 7 : 0),
                  (yy & 7) ^ ((cell & 0x1000) ? 7 : 0), xx, yy, cell >> 14, 0);
        }
    }
}

static void objects(ViewTarget* t, int group, int limited) {
    const ViewBlock* b = t->block;
    int at = b->spt[group], end = group ? b->spt[group - 1] : 1023;
    do {
        const uint16_t* o = b->dram + (0x1e000 + at * 8) / 2;
        unsigned tile = o[3] & 2047;
        if (b->suppress[at] == tile + 1) continue;
        if (!(o[1] & (t->eye ? 0x4000 : 0x8000))) continue;
        unsigned row = (t->y - o[2]) & 255;
        if (row >= 8) continue;
        int parallax = o[1] & 0x3fff;
        int x = sx(o[0] + (t->eye ? parallax : -parallax), 10);
        int shift = t->layout->object_shift[at];
        int min_x = limited ? 0 : -t->margin;
        int max_x = limited ? 383 : t->width - t->margin - 1;
        if (x + shift < min_x - 7 || x + shift > max_x) continue;
        unsigned v = row ^ ((o[3] & 0x1000) ? 7 : 0);
        for (unsigned u = 0; u < 8; ++u)
            pixel(t, x + shift + ((o[3] & 0x2000) ? 7 - (int)u : (int)u),
                  tile, u, v, u, v, o[3] >> 14, 1);
    } while ((at = (at - 1) & 1023) != end);
}

static void extra_sprites(ViewTarget* t) {
    for (unsigned i = 0; i < t->layout->sprite_count && i < VB_VIEWPORT_MAX_SPRITES; ++i) {
        const VbViewportSprite* sprite = &t->layout->sprites[i];
        if (sprite->world != t->world || sprite->eye != t->eye || !sprite->cells || !sprite->chars
            || !sprite->width || !sprite->height || sprite->width > 64 || sprite->height > 64) continue;
        int y = t->y - sprite->y;
        if (y < 0 || y >= (int)sprite->height * 8) continue;
        for (unsigned col = 0; col < sprite->width; ++col) {
            unsigned cell = sprite->cells[(y / 8) * sprite->width + col], tile = cell & 2047;
            if (tile >= sprite->char_count) continue;
            unsigned v = (y & 7) ^ ((cell & 0x1000) ? 7 : 0);
            const uint16_t* artwork = sprite->chars + tile * 8;
            uint32_t hash = 2166136261u;
            const uint8_t* bytes = (const uint8_t*)artwork;
            for (int j = 0; j < 16; ++j) hash = (hash ^ bytes[j]) * 16777619u;
            for (unsigned u = 0; u < 8; ++u) {
                unsigned raw = (artwork[v] >> (u * 2)) & 3;
                int x = sprite->x + (int)col * 8 + ((cell & 0x2000) ? 7 - (int)u : (int)u) + t->margin;
                if (!raw || x < 0 || x >= t->width) continue;
                unsigned at = t->y * t->width + x;
                t->levels[at] = t->block->jplt[cell >> 14][raw];
                t->worlds[at] = t->world + 1;
                t->sources[at] = (VbSourceTexel){hash, u, v, tile, u, v, 255, 3, raw, t->world + 1};
            }
        }
    }
}

int vb_viewport_render(int slot, int eye, int width, uint32_t* argb,
                       uint8_t* levels, uint16_t* worlds, VbSourceTexel* sources) {
    if (slot < 0 || slot > 1 || width < 384 || width > VB_VIEWPORT_MAX_WIDTH
        || (width & 1) || !argb || !levels || !worlds || !sources) return 0;
    for (int row = 0; row < 28; ++row) if (!blocks[slot][row].valid) return 0;
    memset(worlds, 0, width * 224 * sizeof(*worlds));
    memset(sources, 0, width * 224 * sizeof(*sources));
    uint32_t palette[4];
    for (int i = 0; i < 4; ++i) palette[i] = vb_vip_level_argb(i);
    for (int row = 0; row < 28; ++row) {
        const ViewBlock* b = &blocks[slot][row];
        VbViewportScene scene = scene_of(b);
        int kind = classify_scene ? classify_scene(&scene, policy_context) : 0;
        VbViewportLayout layout = {0};
        if (kind && width > 384) arrange_scene(&scene, kind, width, &layout, policy_context);
        ViewTarget target = {0};
        target.block = b; target.layout = &layout; target.width = width;
        target.margin = (width - 384) / 2; target.eye = eye != 0;
        target.levels = levels; target.worlds = worlds; target.sources = sources;
        memset(levels + row * 8 * width, b->backdrop, 8 * width);
        int group = 3;
        for (int world = 31; world >= 0; --world) {
            const uint16_t* w = scene.worlds + world * 16;
            if (w[0] & 64) break;
            target.world = world; target.kind = (w[0] >> 12) & 3;
            target.map = target.kind == 3 ? 255 : w[0] & 15;
            if (w[0] & (eye ? 0x4000 : 0x8000)) {
                for (int y = row * 8; y < row * 8 + 8; ++y) {
                    target.y = y;
                    if (target.kind == 3) objects(&target, group, width > 384 && !kind);
                    else background(&target, w, width > 384 && !kind);
                }
            }
            if (target.kind == 3 && group) --group;
            if (kind && width > 384) for (int y = row * 8; y < row * 8 + 8; ++y) {
                target.y = y; extra_sprites(&target);
            }
        }
    }
    for (int i = 0; i < width * 224; ++i) argb[i] = palette[levels[i] & 3];
    return 1;
}
