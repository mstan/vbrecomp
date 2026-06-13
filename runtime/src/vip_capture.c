/* vip_capture.c — see vip_capture.h.
 *
 * Storage + content-hashing + disk dump for the runtime graphics-capture
 * experiment. The capture *pass* (table walk) is in vip.c; this module is
 * pure instrumentation and never touches the framebuffer. All allocation is
 * lazy and gated on VBRECOMP_CAPTURE, so the default build is byte-identical.
 */
#include "vip_capture.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "png_write.h"

#ifdef _WIN32
#  include <direct.h>
#  define VB_MKDIR(p) _mkdir(p)
#else
#  include <sys/stat.h>
#  define VB_MKDIR(p) mkdir((p), 0777)
#endif

/* ---- capacities (allocated once, only when active) ---- */
#define CAP_MAX_TILES   4096u     /* CHR RAM holds <=2048 distinct chars */
#define CAP_HASH_SLOTS  8192u     /* open-addressing index (power of two) */
#define CAP_USE_RING    131072u   /* draw-use ring, modular eviction */
#define CAP_DUMP_FRAMES 16        /* most-recent frames emitted as layouts */

typedef struct {
    uint32_t hash;
    uint8_t  chr[16];     /* 8 halfwords, little-endian bytes */
    uint32_t first_frame; /* capture frame_seq of first sighting */
    uint32_t use_count;
    uint16_t palette_mask;/* bitmask of palette banks seen (bit p) */
    uint8_t  ctx;         /* first-seen context */
    uint8_t  used;
} TileEntry;

typedef struct {
    VbCaptureUse u;
    uint64_t     seq;     /* global monotonic use counter */
    uint32_t     frame;   /* frame_seq */
} UseRec;

static int      s_active = -1;       /* -1 = unresolved, 0/1 resolved */
static TileEntry* s_tiles = NULL;
static uint32_t s_tile_count = 0;
static int32_t* s_index = NULL;      /* hash slot -> tile idx, -1 empty */
static UseRec*  s_uses = NULL;
static uint64_t s_use_seq = 0;       /* monotonic; modular into s_uses */
static uint32_t s_frame_seq = 0;     /* monotonic capture-frame counter */

int vb_capture_active(void) {
    if (s_active < 0) {
        const char* env = getenv("VBRECOMP_CAPTURE");
        s_active = (env && *env && strcmp(env, "0") != 0) ? 1 : 0;
    }
    return s_active;
}

void vb_capture_init(void) {
    if (!vb_capture_active() || s_tiles) return;
    s_tiles = (TileEntry*)calloc(CAP_MAX_TILES, sizeof(TileEntry));
    s_index = (int32_t*)malloc(CAP_HASH_SLOTS * sizeof(int32_t));
    s_uses  = (UseRec*)calloc(CAP_USE_RING, sizeof(UseRec));
    if (!s_tiles || !s_index || !s_uses) {
        vb_capture_shutdown();
        s_active = 0;   /* allocation failed — disable, stay faithful */
        return;
    }
    for (uint32_t i = 0; i < CAP_HASH_SLOTS; ++i) s_index[i] = -1;
    s_tile_count = 0;
    s_use_seq = 0;
    s_frame_seq = 0;
}

void vb_capture_shutdown(void) {
    free(s_tiles); s_tiles = NULL;
    free(s_index); s_index = NULL;
    free(s_uses);  s_uses  = NULL;
    s_tile_count = 0;
}

void vb_capture_begin_frame(void) {
    if (!vb_capture_active() || !s_tiles) return;
    s_frame_seq++;
}

void vb_capture_end_frame(void) { /* no-op; bracket symmetry */ }

/* FNV-1a over the 16 raw tile bytes. Address-/frame-/position-independent
 * by construction: the input is only the CHR content, stored unflipped. */
static uint32_t fnv1a16(const uint8_t* b) {
    uint32_t h = 2166136261u;
    for (int i = 0; i < 16; ++i) { h ^= b[i]; h *= 16777619u; }
    return h;
}

static void tile_bytes(const uint16_t chr16[8], uint8_t bytes[16]) {
    for (int i = 0; i < 8; ++i) {
        bytes[i * 2 + 0] = (uint8_t)(chr16[i] & 0xFF);
        bytes[i * 2 + 1] = (uint8_t)(chr16[i] >> 8);
    }
}

uint32_t vb_capture_hash(const uint16_t chr16[8]) {
    uint8_t bytes[16];
    tile_bytes(chr16, bytes);
    return fnv1a16(bytes);
}

uint32_t vb_capture_tile(const uint16_t chr16[8], uint8_t ctx, uint8_t palette) {
    if (!vb_capture_active() || !s_tiles) return 0;
    uint8_t bytes[16];
    tile_bytes(chr16, bytes);
    uint32_t h = fnv1a16(bytes);

    /* open-addressing lookup/insert keyed on hash */
    uint32_t slot = h & (CAP_HASH_SLOTS - 1u);
    for (uint32_t probe = 0; probe < CAP_HASH_SLOTS; ++probe) {
        int32_t idx = s_index[slot];
        if (idx < 0) {
            /* insert (unless table full) */
            if (s_tile_count < CAP_MAX_TILES) {
                TileEntry* t = &s_tiles[s_tile_count];
                t->hash = h;
                memcpy(t->chr, bytes, 16);
                t->first_frame = s_frame_seq;
                t->use_count = 0;
                t->palette_mask = (uint16_t)(1u << (palette & 3));
                t->ctx = ctx;
                t->used = 1;
                s_index[slot] = (int32_t)s_tile_count;
                s_tile_count++;
            }
            return h;
        }
        if (s_tiles[idx].hash == h &&
            memcmp(s_tiles[idx].chr, bytes, 16) == 0) {
            s_tiles[idx].palette_mask |= (uint16_t)(1u << (palette & 3));
            s_tiles[idx].use_count++;
            return h;
        }
        slot = (slot + 1u) & (CAP_HASH_SLOTS - 1u);
    }
    return h; /* index full — still return hash for the use record */
}

void vb_capture_use(const VbCaptureUse* use) {
    if (!vb_capture_active() || !s_uses || !use) return;
    UseRec* r = &s_uses[s_use_seq % CAP_USE_RING];
    r->u = *use;
    r->seq = s_use_seq;
    r->frame = s_frame_seq;
    s_use_seq++;
}

/* ---- disk dump ---- */

/* Map a 2bpp tile value 0..3 to grayscale ARGB. Value 0 is transparent so
 * composites reconstruct on a clear background; 1..3 are opaque grey ramp.
 * (Deliberately palette-independent: this is a content preview, not the
 * live brightness-cache render.) No anti-aliasing. */
static uint32_t tile_argb(uint32_t v) {
    static const uint8_t ramp[4] = { 0, 0x55, 0xAA, 0xFF };
    if (v == 0) return 0x00000000u;
    uint8_t g = ramp[v & 3];
    return 0xFF000000u | ((uint32_t)g << 16) | ((uint32_t)g << 8) | g;
}

static void decode_tile(const uint8_t* chr, uint32_t out[64]) {
    for (int row = 0; row < 8; ++row) {
        uint16_t bits = (uint16_t)chr[row * 2] | ((uint16_t)chr[row * 2 + 1] << 8);
        for (int x = 0; x < 8; ++x) {
            uint32_t v = (bits >> (x * 2)) & 3u;   /* low bits = leftmost px */
            out[row * 8 + x] = tile_argb(v);
        }
    }
}

static int write_tile_pngs(const char* tiles_dir, const TileEntry* t) {
    uint32_t px8[64];
    decode_tile(t->chr, px8);

    char path[512];
    snprintf(path, sizeof(path), "%s/%08x.png", tiles_dir, t->hash);
    if (vb_write_png_32bpp(path, 8, 8, px8) != 0) return -1;

    /* 8x nearest-neighbor preview (64x64) for human inspection. */
    static uint32_t px64[64 * 64];
    for (int y = 0; y < 64; ++y)
        for (int x = 0; x < 64; ++x)
            px64[y * 64 + x] = px8[(y / 8) * 8 + (x / 8)];
    snprintf(path, sizeof(path), "%s/%08x@8x.png", tiles_dir, t->hash);
    if (vb_write_png_32bpp(path, 64, 64, px64) != 0) return -1;
    return 0;
}

static const char* ctx_name(uint8_t c) {
    switch (c) {
        case VB_CAP_CTX_OBJ:    return "obj";
        case VB_CAP_CTX_BG:     return "bg";
        case VB_CAP_CTX_AFFINE: return "affine";
        default:                return "unknown";
    }
}

static void write_tile_json(const char* tiles_dir, const TileEntry* t) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%08x.json", tiles_dir, t->hash);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "{\n  \"hash\": \"%08x\",\n", t->hash);
    fprintf(f, "  \"context\": \"%s\",\n", ctx_name(t->ctx));
    fprintf(f, "  \"first_frame\": %u,\n", t->first_frame);
    fprintf(f, "  \"use_count\": %u,\n", t->use_count);
    fprintf(f, "  \"palettes\": [");
    int first = 1;
    for (int p = 0; p < 4; ++p) if (t->palette_mask & (1u << p)) {
        fprintf(f, "%s%d", first ? "" : ", ", p); first = 0;
    }
    fprintf(f, "],\n  \"pixels_2bpp\": [");
    for (int i = 0; i < 16; ++i)
        fprintf(f, "%s%u", i ? ", " : "", (unsigned)t->chr[i]);
    fprintf(f, "]\n}\n");
    fclose(f);
}

static void write_use_json(FILE* f, const VbCaptureUse* u, int first) {
    fprintf(f, "%s\n    {\"tile\": \"%08x\", \"ctx\": \"%s\", \"world\": %u, "
               "\"oam\": %d, \"x_l\": %d, \"x_r\": %d, \"y\": %d, "
               "\"hflip\": %u, \"vflip\": %u, \"vis_l\": %u, \"vis_r\": %u, "
               "\"palette\": %u}",
            first ? "" : ",",
            u->tile_hash, ctx_name(u->ctx), u->world_idx,
            (u->oam_idx == 0xFFFF) ? -1 : (int)u->oam_idx,
            u->x_l, u->x_r, u->y, u->hflip, u->vflip,
            u->vis_l, u->vis_r, u->palette);
}

int vb_capture_dump(const char* dir) {
    if (!vb_capture_active() || !s_tiles) return -1;
    char path[512];

    VB_MKDIR(dir);
    char tiles_dir[400], layouts_dir[400];
    snprintf(tiles_dir, sizeof(tiles_dir), "%s/tiles", dir);
    snprintf(layouts_dir, sizeof(layouts_dir), "%s/layouts", dir);
    VB_MKDIR(tiles_dir);
    VB_MKDIR(layouts_dir);

    /* 1. tile catalog (deduped) */
    for (uint32_t i = 0; i < s_tile_count; ++i) {
        write_tile_pngs(tiles_dir, &s_tiles[i]);
        write_tile_json(tiles_dir, &s_tiles[i]);
    }

    /* 2. per-frame layouts for the most-recent CAP_DUMP_FRAMES frames that
     *    still survive in the use ring. */
    uint32_t newest = s_frame_seq;
    uint32_t oldest = (newest > CAP_DUMP_FRAMES) ? (newest - CAP_DUMP_FRAMES + 1) : 1;
    int frames_written = 0;
    for (uint32_t fr = oldest; fr <= newest; ++fr) {
        /* scan the ring for uses belonging to this frame */
        uint64_t avail = s_use_seq;
        uint64_t from = (avail > CAP_USE_RING) ? (avail - CAP_USE_RING) : 0;
        FILE* f = NULL;
        int first = 1, n = 0;
        for (uint64_t s = from; s < avail; ++s) {
            const UseRec* r = &s_uses[s % CAP_USE_RING];
            if (r->frame != fr) continue;
            if (!f) {
                snprintf(path, sizeof(path), "%s/frame_%06u.json", layouts_dir, fr);
                f = fopen(path, "wb");
                if (!f) break;
                fprintf(f, "{\n  \"frame\": %u,\n  \"uses\": [", fr);
            }
            write_use_json(f, &r->u, first);
            first = 0; n++;
        }
        if (f) {
            fprintf(f, "\n  ],\n  \"use_count\": %d\n}\n", n);
            fclose(f);
            frames_written++;
        }
    }

    /* 3. session.json */
    snprintf(path, sizeof(path), "%s/session.json", dir);
    FILE* sf = fopen(path, "wb");
    if (sf) {
        fprintf(sf,
            "{\n"
            "  \"hash_policy\": \"FNV-1a over 16 raw 2bpp bytes of the "
            "UNFLIPPED 8x8 tile; flips recorded separately per use\",\n"
            "  \"tile_count\": %u,\n"
            "  \"use_seq_total\": %llu,\n"
            "  \"frame_seq_total\": %u,\n"
            "  \"layout_frames_written\": %d,\n"
            "  \"tile_png\": \"grayscale ramp, value 0 = transparent, no AA; "
            "<hash>.png is 8x8, <hash>@8x.png is 64x64 nearest-neighbor\"\n"
            "}\n",
            s_tile_count, (unsigned long long)s_use_seq, s_frame_seq,
            frames_written);
        fclose(sf);
    }

    return (int)s_tile_count;
}
