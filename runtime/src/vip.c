/* vip.c — Virtual Image Processor.
 *
 * The VIP exposes its character RAM, BG cell maps, OBJ tables, world
 * descriptors, brightness control table, and registers in a 512 KB
 * window (0x00000000..0x0007FFFF), mirrored across the 16 MB bank.
 *
 * Two layers:
 *   1. Flat 512 KB shadow — backs CHR RAM, BG/OBJ/WORLD DRAM, and the
 *      L/R framebuffers. Reads return whatever the cart most recently
 *      wrote. Mirror folds via `vip_offset()`. This was P3 territory.
 *   2. P4-B: a column-driven state machine for the register page at
 *      0x5F800..0x5F8FF. Reads compute live values
 *      (DPSTTS/XPSTTS/INTPND); writes drive control bits and
 *      may latch IRQs. Driven by `vb_vip_tick(cycles)` from main.cpp.
 *
 * Behavioural oracle: Beetle VB (beetle-vb/mednafen/vb/vip.c) —
 *   VIP_Power line 399  : initial state
 *   VIP_Update line 1286: 259-cycle/column state machine
 *   ReadRegister line 456 / WriteRegister line 554: register R/W
 *   CheckIRQ line 375   : VBIRQ_SOURCE_VIP assertion gate
 *
 * The state machine does NOT yet draw pixels — that's P4-C. P4-B
 * lights up just enough timing for Mario's Tennis's INTPND-poll
 * loop to observe nonzero values and progress past 0xFFF81AF2.
 */
#include "vip.h"

#include <math.h>
#include <string.h>

#include "input.h"
#include "interrupts.h"
#include "memory.h"      /* vb_memory_dump — WRAM observation ring */
#include "red_lut.h"
#include "stub_abort.h"
#include "vip_capture.h"
#include "vip_phase.h"
#include "wram_hash.h"
#include "asset_pack.h"
#include "recolor.h"
#include "renderer.h"
#include "mod_runtime.h"

#define VIP_WINDOW_SIZE 0x80000u
#define VIP_REGISTER_BASE 0x5F800u
#define VIP_REGISTER_END  0x5F900u

#define VIP_CYCLES_PER_COLUMN 259
#define VIP_COLUMNS_PER_PHASE 384

#define XPCTRL_XP_RST 0x0001u
#define XPCTRL_XP_EN  0x0002u

#define DPCTRL_DPRST  0x0001u
#define DPCTRL_DISP   0x0002u

static uint8_t s_vip_mem[VIP_WINDOW_SIZE];

/* Register-state ground truth (Beetle equivalents in parentheses). */
static uint16_t s_intpnd;          /* InterruptPending */
static uint16_t s_intenb;          /* InterruptEnable */
static uint16_t s_dpctrl;          /* DPCTRL */
static uint16_t s_xpctrl;          /* XPCTRL */
static uint16_t s_sbcmp;           /* SBCMP — derived from XPCTRL high byte on write */
static uint16_t s_frmcyc;          /* FRMCYC */
static uint16_t s_bkcol;           /* BKCOL */
static uint8_t  s_brta, s_brtb, s_brtc, s_rest;
static uint16_t s_spt[4];
static uint16_t s_gplt[4];
static uint16_t s_jplt[4];

/* Per-palette caches: 4 sub-values × 4 palette banks, each
 * `(GPLT[bank] >> (sub*2)) & 3` (Beetle vip.c:338-353). */
static uint8_t s_gplt_cache[4][4];
static uint8_t s_jplt_cache[4][4];

/* Per-pixel brightness (0..255) for 2bpp framebuffer values 0..3.
 * Beetle vip.c:172-223 RecalcBrightnessCache. Output is a cumulative
 * "on-time" fraction of MaxTime=128, scaled to 0..255. Used by the
 * screenshot path to convert 2bpp pixels → grayscale ARGB. */
static int32_t s_brt_cache[4];
static uint8_t s_brt_repeat;   /* per-frame "Repeat" — set from CT data */

/* Beetle's MakeColorLUT (vip.c:108-144) applies a 1/2.2 gamma curve
 * when mapping the BrightnessCache scalar (0..255) into the host
 * framebuffer's RGB channel. Default_Color is 0xFFFFFF in mednafen-vb
 * (vip.c:389) so the per-channel scale collapses to 1.0; the gamma
 * step is what raises BrightnessCache[3]=199 to host R=227 = 0xE3. */
static uint8_t s_color_lut[256];
static int     s_color_lut_built;

static void build_color_lut(void) {
    for (int i = 0; i < 256; ++i) {
        double prod = (double)i / 255.0;
        double r_prime = pow(prod, 1.0 / 2.2);
        int v = (int)(r_prime * 255.0);
        if (v < 0) v = 0;
        if (v > 255) v = 255;
        s_color_lut[i] = (uint8_t)v;
    }
    s_color_lut_built = 1;
}

static void recalc_gplt_cache(int which) {
    for (int i = 0; i < 4; ++i)
        s_gplt_cache[which][i] = (uint8_t)((s_gplt[which] >> (i * 2)) & 3u);
}
static void recalc_jplt_cache(int which) {
    for (int i = 0; i < 4; ++i)
        s_jplt_cache[which][i] = (uint8_t)((s_jplt[which] >> (i * 2)) & 3u);
}

/* Port of Beetle RecalcBrightnessCache (vip.c:172-223). Computes
 * the time-integral of each brightness phase across the BRT cycle,
 * scaled to 0..255. */
static void recalc_brt_cache(void) {
    int32_t cumulative = (int32_t)s_brta + 1 + (int32_t)s_brtb + 1
                       + (int32_t)s_brtc + 1 + (int32_t)s_rest + 1 + 1;
    const int32_t max_time = 128;
    int32_t b[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < s_brt_repeat + 1; ++i) {
        if (i * cumulative >= max_time) break;
        int32_t b1 = i * cumulative + (int32_t)s_brta;
        if (b1 > max_time) b1 = max_time;
        b1 -= i * cumulative;
        if (b1 < 0) b1 = 0;

        int32_t b2 = i * cumulative + (int32_t)s_brta + 1 + (int32_t)s_brtb;
        if (b2 > max_time) b2 = max_time;
        b2 -= i * cumulative + (int32_t)s_brta + 1;
        if (b2 < 0) b2 = 0;

        int32_t b3 = i * cumulative + (int32_t)s_brta + (int32_t)s_brtb
                   + (int32_t)s_brtc + 1;
        if (b3 > max_time) b3 = max_time;
        b3 -= i * cumulative + 1;
        if (b3 < 0) b3 = 0;

        b[1] += b1;
        b[2] += b2;
        b[3] += b3;
    }
    s_brt_cache[0] = 0;
    for (int i = 1; i < 4; ++i)
        s_brt_cache[i] = 255 * b[i] / max_time;
}

/* Column-machine state (Beetle equivalents in parentheses). */
static int32_t  s_column;                /* Column */
static int32_t  s_column_counter;        /* ColumnCounter */
static int32_t  s_display_region;        /* DisplayRegion 0..3 */
static int      s_display_active;        /* DisplayActive */
static int      s_display_fb;            /* DisplayFB */
static int32_t  s_game_frame_counter;    /* GameFrameCounter */
static int32_t  s_drawing_counter;       /* DrawingCounter */
static int      s_drawing_active;        /* DrawingActive */
static int      s_drawing_fb;            /* DrawingFB */
static int32_t  s_drawing_block;         /* DrawingBlock 0..27 */
static int32_t  s_sb_latch;              /* SB_Latch */
static int64_t  s_sbout_inactive_time;   /* SBOUT_InactiveTime — VIP-cycle absolute */
static uint64_t s_vip_cycles;            /* monotonic VIP cycle counter */

/* ---- Always-on observation rings (experiment / collaborative capture) ----
 * Sized to span a whole play session, frame-stamped, walked AFTER the fact —
 * never a short window a probe must race (mirrors snesrecomp's multi-minute
 * frame history + WRAM anchors). The user plays freely, pauses whenever, and we
 * query the recorded history; there is no "arm then catch the event" timing.
 * Both keyed by a per-displayed-frame sequence number (s_vip_frame_seq, bumped
 * at the framebuffer flip), so an anchor's WRAM aligns to a world-ring frame.
 * They fill only while the recolor render runs (overrides on, same precondition
 * as world_map — the only path that builds the attribution buffer).
 *   - World ring: per displayed frame, the active-world mask + each present
 *     world's on-screen bbox. ~11 min deep, for finding e.g. which world draws
 *     the far opponent during a rally.
 *   - WRAM anchor ring: a game-state WRAM-window snapshot taken at each
 *     world-set TRANSITION (one entry per distinct on-screen state, so a whole
 *     match's states fit). Diff the window across anchors with different masks
 *     to find the sub-state byte. */
static uint32_t s_vip_frame_seq = 0;     /* monotonic displayed-frame counter */

#define WRING_FRAMES 32768               /* ~11 min at 50 Hz (~11 MB) */
#define WRING_MAXW   20
typedef struct { uint8_t world; int16_t x0, y0, x1, y1; uint32_t count; } WRingBox;
typedef struct { uint32_t seq, mask; uint8_t nbox; WRingBox box[WRING_MAXW]; } WRingFrame;
static WRingFrame s_wring[WRING_FRAMES];
static uint32_t   s_wring_head, s_wring_count, s_wring_last_seq = 0xFFFFFFFFu;

#define ARING_N    1024                  /* distinct-state anchors (~2 MB) */
#define ARING_BASE 0x05002000u           /* match/player struct lives here */
#define ARING_LEN  0x800u                /* 2 KB game-state window */
typedef struct { uint32_t seq, mask; uint8_t buf[ARING_LEN]; } ARingFrame;
static ARingFrame s_aring[ARING_N];
static uint32_t   s_aring_head, s_aring_count, s_aring_last_mask = 0xFFFFFFFFu;
static uint32_t   s_prev_active_mask = 0; /* previous frame's raw mask (flicker) */

/* Record one displayed frame into both rings. Deduped on the frame seq so the
 * per-eye / per-screenshot calls into the recolor render append only once.
 * wcnt/wy0/.. are indexed by attr value a (1..32); world == a-1. The WRAM
 * anchor is taken only when the active-world mask changes. */
static void vip_rings_record(uint32_t mask, const int* wy0, const int* wy1,
                             const int* wx0, const int* wx1, const int* wcnt) {
    if (s_vip_frame_seq == s_wring_last_seq) return;
    s_wring_last_seq = s_vip_frame_seq;

    WRingFrame* f = &s_wring[s_wring_head];
    f->seq = s_vip_frame_seq; f->mask = mask; f->nbox = 0;
    for (int a = 1; a <= 32 && f->nbox < WRING_MAXW; ++a) {
        if (wcnt[a] < 16 || wy1[a] < 0) continue;
        WRingBox* b = &f->box[f->nbox++];
        b->world = (uint8_t)(a - 1);
        b->x0 = (int16_t)wx0[a]; b->y0 = (int16_t)wy0[a];
        b->x1 = (int16_t)wx1[a]; b->y1 = (int16_t)wy1[a];
        b->count = (uint32_t)wcnt[a];
    }
    s_wring_head = (s_wring_head + 1) % WRING_FRAMES;
    if (s_wring_count < WRING_FRAMES) s_wring_count++;

    /* Canonicalize across the alternating-frame redraw: the VB toggles sprite
     * worlds (near player, net, ball) every other frame, so the raw mask flips
     * each frame and would make EVERY frame a transition — flooding the anchor
     * ring and evicting real states within ~10 s. OR two consecutive frames so a
     * steady scene yields ONE stable signature; anchor only when that changes. */
    uint32_t canon = mask | s_prev_active_mask;
    s_prev_active_mask = mask;
    if (canon != s_aring_last_mask) {
        s_aring_last_mask = canon;
        ARingFrame* an = &s_aring[s_aring_head];
        an->seq = s_vip_frame_seq; an->mask = canon;
        vb_memory_dump(ARING_BASE, an->buf, ARING_LEN);
        s_aring_head = (s_aring_head + 1) % ARING_N;
        if (s_aring_count < ARING_N) s_aring_count++;
    }
}


static uint32_t vip_offset(uint32_t addr) {
    return addr & (VIP_WINDOW_SIZE - 1u);
}

/* CHR-RAM aliasing. Beetle exposes the same 32 KB CHR-RAM via two
 * access surfaces (vip.c:670-686, 715-716):
 *   - Bank 0/1 alias windows at 0x06000-0x07FFF, 0x0E000-0x0FFFF,
 *     0x16000-0x17FFF, 0x1E000-0x1FFFF. The 4 windows tile the 32 KB
 *     via index `(A & 0x1FFF) | ((A >> 2) & 0x6000)`.
 *   - Bank 7 canonical window at 0x78000-0x7FFFF: `A & 0x7FFF`.
 *
 * Both surfaces share storage. We route everything to the canonical
 * 0x78000+ slot; the alias regions in s_vip_mem are unused. Without
 * this, a cart that writes char data through the alias window
 * (some carts do; Mario's Tennis uses both) would invisibly miss
 * the renderer's CHR fetch path at 0x78000. */
#define VIP_CHR_CANONICAL_BASE 0x78000u
#define VIP_CHR_CANONICAL_SIZE 0x8000u   /* 32 KB CHR_RAM */

static int vip_alias_index(uint32_t offset, uint32_t* out_canonical) {
    /* Bank-0/1 alias windows have (offset & 0x7FFF) >= 0x6000. */
    if (offset < 0x20000u && (offset & 0x7FFFu) >= 0x6000u) {
        *out_canonical = VIP_CHR_CANONICAL_BASE
                       | ((offset & 0x1FFFu)
                          | ((offset >> 2) & 0x6000u));
        return 1;
    }
    /* Bank 7 (0x70000-0x7FFFF) is the canonical window; the
     * low-half mirrors (0x70000-0x77FFF) map to the same bytes. */
    if (offset >= 0x70000u && offset < 0x80000u) {
        *out_canonical = VIP_CHR_CANONICAL_BASE | (offset & 0x7FFFu);
        return 1;
    }
    return 0;
}

static void vip_check_irq(void) {
    vb_irq_assert(VBIRQ_SOURCE_VIP, (s_intpnd & s_intenb) ? 1 : 0);
}

/* Forward decl — renderer is defined later in the file, but the
 * column state machine calls it. */
static void vip_draw_block_now(uint8_t block_no);

/* Forward decl — opt-in graphics-capture pass (experiment). Defined after
 * the renderer; called from the column machine at drawing-frame start. A
 * no-op when VBRECOMP_CAPTURE is unset (gated at the call site). */
static void vip_capture_pass(void);

/* Forward decl — opt-in override resolve pass (experiment). Builds the
 * per-OAM suppress set + per-eye overlay draw-list for the frame about to be
 * drawn. Gated on VBRECOMP_OVERRIDES at the call site. */
static void vip_resolve_overrides(void);

/* Recolor attribution (experiment): per-pixel value = (world index + 1) of the
 * world that drew the pixel; 0 = none/background. Captured during draw and
 * packed into a full-res per-eye buffer (double-buffered by drawing_fb).
 * Keying on WORLD (not tile content) keeps a character colored across its whole
 * animation. Written only when attribution is active; the 2bpp framebuffer
 * output is identical either way (separate memory), so a faithful run is
 * byte-identical. */
static int      s_attr_on;
static uint16_t s_attr_fb[2][2][384 * 224];  /* [fb_slot][eye][y*384 + x]; world+1 */
static uint16_t s_attr_val;                   /* world+1 of the world being drawn */
static uint16_t s_attr_row[2][8 * 512];
static VbSourceTexel s_source_row[2][8 * 512];
static VbSourceTexel s_source_fb[2][2][384 * 224];
static uint32_t s_source_hash[2048];
static uint8_t s_source_hashed[2048];
static uint8_t s_source_map, s_source_kind;


/* Per-OAM suppression for matched OBJ tiles: 0 = draw faithfully; otherwise
 * (char_no + 1) of the matched tile, re-checked in draw_obj so a mid-frame
 * OAM mutation falls back to the original 2bpp render. All-zero (and never
 * written) unless VBRECOMP_OVERRIDES is active ⇒ byte-identical when off. */
static uint16_t s_obj_suppress[1024];


void vb_vip_init(void) {
    memset(s_vip_mem, 0, sizeof(s_vip_mem));
    memset(s_source_fb, 0, sizeof(s_source_fb));
    vb_vip_phase_reset();
    vb_wram_hash_reset();
    /* Beetle VIP_Power (vip.c:399). */
    s_intpnd = 0;
    s_intenb = 0;
    s_dpctrl = DPCTRL_DISP;            /* Beetle sets DPCTRL = 2 at power */
    s_xpctrl = 0;
    s_sbcmp  = 0;
    s_frmcyc = 0;
    s_bkcol  = 0;
    s_brta = s_brtb = s_brtc = s_rest = 0;
    s_brt_repeat = 0;
    build_color_lut();
    recalc_brt_cache();
    for (int i = 0; i < 4; ++i) {
        s_spt[i] = 0; s_gplt[i] = 0; s_jplt[i] = 0;
        recalc_gplt_cache(i);
        recalc_jplt_cache(i);
    }
    s_column         = 0;
    s_column_counter = VIP_CYCLES_PER_COLUMN;
    s_display_region = 0;
    s_display_active = 0;
    s_display_fb     = 0;
    s_game_frame_counter = 0;
    s_drawing_counter = 0;
    s_drawing_active  = 0;
    s_drawing_fb      = 0;
    s_drawing_block   = 0;
    s_sb_latch        = 0;
    s_vip_frame_seq   = 0;
    s_wring_head = s_wring_count = 0; s_wring_last_seq = 0xFFFFFFFFu;
    s_aring_head = s_aring_count = 0; s_aring_last_mask = 0xFFFFFFFFu;
    s_prev_active_mask = 0;
    s_sbout_inactive_time = -1;
    s_vip_cycles = 0;
    vb_capture_init();      /* allocates rings only if VBRECOMP_CAPTURE is set */
    vb_overrides_init();    /* loads pack only if VBRECOMP_OVERRIDES is set */
    vb_recolor_init();      /* loads recolor pack only if present */
    s_attr_on = vb_recolor_active() || vb_capture_active() || vb_renderer_tracks_worlds() || vb_renderer_active();
    memset(s_obj_suppress, 0, sizeof(s_obj_suppress));
    vip_check_irq();
}

void vb_vip_shutdown(void) {}


/* ----------------- Column state machine ----------------- */

static void vip_advance_column(void) {
    /* CT-table sample, Beetle vip.c:1357-1366. Every 4 columns during
     * an active display half, DRAM[0x1DFFE - (Column>>2)*2 - (lr?0:0x200)]
     * is fetched; the high byte is the per-column brightness "Repeat"
     * count consumed by recalc_brt_cache(). Without this the loop body
     * in recalc_brt_cache runs once instead of Repeat+1 times and every
     * lit pixel comes out 24/255 too dark vs the oracle. */
    if ((s_display_region & 1) && !(s_column & 3)) {
        int lr = (s_display_region & 2) >> 1;
        uint32_t ct_off = 0x1DFFEu
                       - (uint32_t)((s_column >> 2) * 2)
                       - (lr ? 0u : 0x200u);
        uint32_t flat = 0x20000u + ct_off;
        uint16_t ctdata = (uint16_t)s_vip_mem[flat]
                        | ((uint16_t)s_vip_mem[flat + 1] << 8);
        uint8_t repeat = (uint8_t)(ctdata >> 8);
        if (repeat != s_brt_repeat) {
            s_brt_repeat = repeat;
            recalc_brt_cache();
        }
    }

    s_column++;
    if (s_column == VIP_COLUMNS_PER_PHASE) {
        s_column = 0;

        /* End of an active display half — fire L/R FB_END. */
        if (s_display_active && (s_display_region & 1)) {
            uint16_t fbend = (s_display_region & 2) ? VB_VIP_INT_RFB_END
                                                    : VB_VIP_INT_LFB_END;
            s_intpnd |= fbend;
            vip_check_irq();
            vb_vip_phase_record(fbend, vb_vip_dpstts(), vb_vip_xpstts());
        }

        s_display_region = (s_display_region + 1) & 3;

        if (s_display_region == 0) {
            /* New display frame. Sample DPCTRL.DISP. */
            s_display_active = (s_dpctrl & DPCTRL_DISP) ? 1 : 0;
            if (s_display_active) {
                s_intpnd |= VB_VIP_INT_FRAME_START;
                vip_check_irq();
                vb_vip_phase_record(VB_VIP_INT_FRAME_START,
                                    vb_vip_dpstts(), vb_vip_xpstts());
            }

            /* Game-frame divider per FRMCYC. */
            s_game_frame_counter++;
            if (s_game_frame_counter > (int32_t)s_frmcyc) {
                s_intpnd |= VB_VIP_INT_GAME_START;
                vb_mod_runtime_frame_tick_c();
                s_attr_on = vb_recolor_active() || vb_capture_active() || vb_renderer_tracks_worlds() || vb_renderer_active();
                vip_check_irq();
                vb_vip_phase_record(VB_VIP_INT_GAME_START,
                                    vb_vip_dpstts(), vb_vip_xpstts());
                /* Axis-6 whole-session fidelity: fingerprint WRAM at the
                 * game-frame boundary (1:1-aligned with the oracle via the
                 * proven GAME_START sequence). */
                {
                    uint32_t wr_fnv[VB_WRAM_FNV_REGIONS];
                    vb_wram_region_fnv(wr_fnv);
                    vb_wram_hash_record(wr_fnv);
                }

                /* Advance any frame-counted debug press once per game frame
                 * (deterministic headless navigation; auto-releases). */
                vb_input_frame_advance();

                if (s_xpctrl & XPCTRL_XP_EN) {
                    s_display_fb     = s_drawing_fb;
                    s_drawing_fb    ^= 1;
                    s_drawing_block  = 0;
                    s_drawing_active = 1;
                    s_drawing_counter = 1120 * 4;

                    /* New displayed frame: bump the observation-ring sequence.
                     * Both rings are recorded later by the recolor render
                     * (which has the attribution buffer), keyed off this seq. */
                    s_vip_frame_seq++;

                    /* Opt-in capture: snapshot the world/OAM/CHR tables the
                     * about-to-draw blocks will read. Gated on
                     * VBRECOMP_CAPTURE — a single predictable branch when
                     * off, so the faithful path is unchanged. */
                    if (vb_capture_active()) vip_capture_pass();

                    /* Opt-in override resolve: build the suppress set +
                     * overlay list for this drawing frame. Gated on
                     * VBRECOMP_OVERRIDES; faithful when off. */
                    if (vb_overrides_active()) vip_resolve_overrides();
                }
                s_game_frame_counter = 0;
            }
        }
    }
}

static void vip_advance_chunk(int32_t chunk_clocks) {
    s_vip_cycles += (uint64_t)chunk_clocks;

    /* Drawing block timing — every 1120*4 cycles a block draws into
     * the back-buffer FB; after 28 blocks XP_END fires.
     *
     * Order matches Beetle (vip.c:1302-1349): when the counter
     * reaches 0 we draw the CURRENT block, then advance. */
    if (s_drawing_counter > 0) {
        s_drawing_counter -= chunk_clocks;
        if (s_drawing_counter <= 0) {
            vip_draw_block_now((uint8_t)s_drawing_block);
            /* SBOUT goes inactive 1120 cycles into the next column. */
            s_sbout_inactive_time = (int64_t)s_vip_cycles + 1120;
            s_sb_latch = s_drawing_block;
            s_drawing_block++;
            if (s_drawing_block == 28) {
                s_drawing_active = 0;
                s_intpnd |= VB_VIP_INT_XP_END;
                vip_check_irq();
                vb_vip_phase_record(VB_VIP_INT_XP_END,
                                    vb_vip_dpstts(), vb_vip_xpstts());
            } else {
                s_drawing_counter += 1120 * 4;
            }
        }
    }

    s_column_counter -= chunk_clocks;
    if (s_column_counter == 0) {
        s_column_counter = VIP_CYCLES_PER_COLUMN;
        vip_advance_column();
    }
}

void vb_vip_tick(uint64_t cycles) {
    int64_t clocks = (int64_t)cycles;
    while (clocks > 0) {
        int32_t chunk = (int32_t)clocks;
        if (s_drawing_counter > 0 && chunk > s_drawing_counter)
            chunk = s_drawing_counter;
        if (chunk > s_column_counter)
            chunk = s_column_counter;
        if (chunk <= 0) chunk = 1;
        vip_advance_chunk(chunk);
        clocks -= chunk;
    }
}

/* Cycles until the VIP's next state-machine boundary (column tick or
 * drawing-block boundary) — i.e. the next point at which it could set an
 * INTPND bit (FB_END / FRAME_START / GAME_START / XP_END). The event-
 * driven idle loop (main.cpp HALT path) steps device time by this so an
 * IRQ lands within ~one column (259 cyc) instead of a fixed 20000-cycle
 * chunk. Always >= 1. */
int32_t vb_vip_cycles_to_next_event(void) {
    int32_t n = s_column_counter;
    if (s_drawing_counter > 0 && s_drawing_counter < n)
        n = s_drawing_counter;
    return n < 1 ? 1 : n;
}


/* ----------------- Register read/write ----------------- */

static uint16_t vip_read_register16(uint32_t offset) {
    /* Beetle ReadRegister (vip.c:456). The register page lives at
     * VIP_REGISTER_BASE; Beetle's switch masks A & 0xFE so the sub-
     * offset within the page is what matters. */
    uint32_t sub = (offset - VIP_REGISTER_BASE) & 0xFEu;
    switch (sub) {
        case 0x00: return s_intpnd;
        case 0x02: return s_intenb;
        case 0x04: return 0; /* INTCLR write-only */

        case 0x20: {
            /* DPSTTS: bits 8/9 (DPBSY-L0/L1), 10/11 (DPBSY-R0/R1)
             * follow DisplayRegion + DisplayFB; bit 6 always set; the
             * low DPCTRL bits mirror through. Beetle vip.c:470-485. */
            uint16_t ret = s_dpctrl & 0x702u;
            if ((s_display_region & 1) && s_display_active) {
                unsigned dpbsy = 1u << ((s_display_region >> 1) & 1);
                if (s_display_fb) dpbsy <<= 2;
                ret |= (uint16_t)(dpbsy << 2);
            }
            ret |= 1u << 6;
            return ret;
        }
        case 0x22: return s_dpctrl;
        case 0x24: return (uint16_t)s_brta;
        case 0x26: return (uint16_t)s_brtb;
        case 0x28: return (uint16_t)s_brtc;
        case 0x2A: return (uint16_t)s_rest;
        case 0x2E: return s_frmcyc;
        case 0x30: return 0xFFFFu;        /* CTA — Beetle returns 0xFFFF */

        case 0x40: {
            /* XPSTTS: XPCTRL.bit1 + DrawingFB-shifted-if-active + SBOUT
             * indicator + SB_Latch in high byte. Beetle vip.c:508-518. */
            uint16_t ret = (uint16_t)(s_xpctrl & 0x2u);
            if (s_drawing_active) {
                ret |= (uint16_t)((1 + s_drawing_fb) << 2);
            }
            if ((int64_t)s_vip_cycles < s_sbout_inactive_time) {
                ret |= 0x8000u;
                ret |= (uint16_t)((s_sb_latch & 0xFFu) << 8);
            }
            return ret;
        }
        case 0x42: return s_xpctrl;
        case 0x44: return 2;             /* VIP version (Beetle hardcodes 2) */

        case 0x48: case 0x4A: case 0x4C: case 0x4E:
            return s_spt[(sub >> 1) & 3];

        case 0x60: case 0x62: case 0x64: case 0x66:
            return s_gplt[(sub >> 1) & 3];

        case 0x68: case 0x6A: case 0x6C: case 0x6E:
            return s_jplt[(sub >> 1) & 3];

        case 0x70: return s_bkcol;
        default:
            /* Other register-page reads default to 0 (Beetle's switch
             * falls through and returns 0). Real cart accesses should
             * surface here as 0; they don't abort. */
            return 0;
    }
}

static void vip_write_register16(uint32_t offset, uint16_t v) {
    uint32_t sub = (offset - VIP_REGISTER_BASE) & 0xFEu;
    switch (sub) {
        case 0x00:
            /* INTPND read-only on write (Beetle vip.c:559). */
            break;
        case 0x02:
            s_intenb = v & VB_VIP_INT_MASK_VALID;
            vip_check_irq();
            break;
        case 0x04:
            /* INTCLR: write-1-to-clear. Beetle vip.c:565-567. */
            s_intpnd &= ~v;
            vip_check_irq();
            break;
        case 0x20:
            /* DPSTTS read-only. */
            break;
        case 0x22:
            /* DPCTRL: bit0 = display reset → clear active + display IRQs. */
            s_dpctrl = v & 0x703u;
            if (v & DPCTRL_DPRST) {
                s_display_active = 0;
                s_intpnd &= (uint16_t)~(VB_VIP_INT_TIME_ERR
                                      | VB_VIP_INT_FRAME_START
                                      | VB_VIP_INT_GAME_START
                                      | VB_VIP_INT_RFB_END
                                      | VB_VIP_INT_LFB_END
                                      | VB_VIP_INT_SCANERR);
                vip_check_irq();
            }
            break;
        case 0x24: s_brta = (uint8_t)(v & 0xFFu); recalc_brt_cache(); break;
        case 0x26: s_brtb = (uint8_t)(v & 0xFFu); recalc_brt_cache(); break;
        case 0x28: s_brtc = (uint8_t)(v & 0xFFu); recalc_brt_cache(); break;
        case 0x2A: s_rest = (uint8_t)(v & 0xFFu); recalc_brt_cache(); break;
        case 0x2E: s_frmcyc = v & 0xFu; break;
        case 0x30: /* CTA read-only */ break;
        case 0x40: /* XPSTTS read-only */ break;
        case 0x42:
            /* XPCTRL: low bits = enable; high byte = SBCMP. bit0 reset
             * flips frame buffer + clears SB/XP/TIME IRQs. */
            s_xpctrl = v & 0x0002u;
            s_sbcmp  = (uint16_t)((v >> 8) & 0x1Fu);
            if (v & XPCTRL_XP_RST) {
                s_drawing_fb     = s_display_fb;
                s_display_fb    ^= 1;
                s_drawing_active = 0;
                s_drawing_counter = 0;
                s_intpnd &= (uint16_t)~(VB_VIP_INT_SB_HIT
                                      | VB_VIP_INT_XP_END
                                      | VB_VIP_INT_TIME_ERR);
                vip_check_irq();
            }
            break;
        case 0x44: /* VIP version read-only */ break;
        case 0x48: case 0x4A: case 0x4C: case 0x4E:
            s_spt[(sub >> 1) & 3] = v & 0x3FFu;
            break;
        case 0x60: case 0x62: case 0x64: case 0x66: {
            int which = (sub >> 1) & 3;
            s_gplt[which] = v & 0xFCu;
            recalc_gplt_cache(which);
            break;
        }
        case 0x68: case 0x6A: case 0x6C: case 0x6E: {
            int which = (sub >> 1) & 3;
            s_jplt[which] = v & 0xFCu;
            recalc_jplt_cache(which);
            break;
        }
        case 0x70:
            s_bkcol = v & 0x3u;
            break;
        default:
            /* Other register-page writes are ignored (Beetle falls
             * through). The shadow update below in vb_vip_write*
             * still happens so subsequent reads observe what was
             * written. */
            break;
    }
}


/* ----------------- Bus surface ----------------- */

static int vip_is_register(uint32_t offset) {
    return offset >= VIP_REGISTER_BASE && offset < VIP_REGISTER_END;
}

/* Route to canonical CHR storage when offset is in an alias window;
 * otherwise return offset unchanged. */
static uint32_t vip_route(uint32_t offset) {
    uint32_t canonical;
    if (vip_alias_index(offset, &canonical)) return canonical;
    return offset;
}

uint8_t vb_vip_read8(uint32_t addr) {
    uint32_t o = vip_offset(addr);
    if (vip_is_register(o)) {
        uint16_t r = vip_read_register16(o & ~1u);
        return (o & 1u) ? (uint8_t)(r >> 8) : (uint8_t)(r & 0xFF);
    }
    return s_vip_mem[vip_route(o)];
}

uint16_t vb_vip_read16(uint32_t addr) {
    uint32_t o = vip_offset(addr) & ~1u;
    if (vip_is_register(o)) {
        return vip_read_register16(o);
    }
    uint32_t r = vip_route(o);
    return (uint16_t)s_vip_mem[r]
         | ((uint16_t)s_vip_mem[r + 1] << 8);
}

uint32_t vb_vip_read32(uint32_t addr) {
    uint32_t o = vip_offset(addr) & ~3u;
    if (vip_is_register(o)) {
        uint32_t lo = vip_read_register16(o);
        uint32_t hi = vip_read_register16(o + 2);
        return lo | (hi << 16);
    }
    uint32_t r = vip_route(o);
    return  (uint32_t)s_vip_mem[r]
         | ((uint32_t)s_vip_mem[r + 1] << 8)
         | ((uint32_t)s_vip_mem[r + 2] << 16)
         | ((uint32_t)s_vip_mem[r + 3] << 24);
}

void vb_vip_write8(uint32_t addr, uint8_t v) {
    uint32_t o = vip_offset(addr);
    if (vip_is_register(o)) {
        /* Beetle's WriteRegister is halfword-oriented. Promote the
         * byte to a halfword preserving the other byte from the
         * current live value. */
        uint16_t current = vip_read_register16(o & ~1u);
        uint16_t merged;
        if (o & 1u) merged = (uint16_t)((current & 0x00FFu) | ((uint16_t)v << 8));
        else        merged = (uint16_t)((current & 0xFF00u) | v);
        vip_write_register16(o & ~1u, merged);
        return;
    }
    s_vip_mem[vip_route(o)] = v;
}

void vb_vip_write16(uint32_t addr, uint16_t v) {
    uint32_t o = vip_offset(addr) & ~1u;
    if (vip_is_register(o)) {
        vip_write_register16(o, v);
        return;
    }
    uint32_t r = vip_route(o);
    s_vip_mem[r]     = (uint8_t)(v & 0xFFu);
    s_vip_mem[r + 1] = (uint8_t)((v >> 8) & 0xFFu);
}

void vb_vip_write32(uint32_t addr, uint32_t v) {
    uint32_t o = vip_offset(addr) & ~3u;
    if (vip_is_register(o)) {
        vip_write_register16(o,     (uint16_t)(v & 0xFFFFu));
        vip_write_register16(o + 2, (uint16_t)((v >> 16) & 0xFFFFu));
        return;
    }
    uint32_t r = vip_route(o);
    s_vip_mem[r]     = (uint8_t)(v & 0xFFu);
    s_vip_mem[r + 1] = (uint8_t)((v >> 8) & 0xFFu);
    s_vip_mem[r + 2] = (uint8_t)((v >> 16) & 0xFFu);
    s_vip_mem[r + 3] = (uint8_t)((v >> 24) & 0xFFu);
}


/* ----------------- Renderer ----------------- *
 *
 * Faithful port of beetle-vb/mednafen/vb/vip_draw.inc (437 LOC):
 *   DrawBG      vip_draw.inc:7    — normal / HBias BG
 *   DrawAffine  vip_draw.inc:111  — affine BG (bgm == 2)
 *   DrawOBJ     vip_draw.inc:263  — sprite groups
 *   VIP_DrawBlock vip_draw.inc:352 — top-level 8-row block; walks 32 worlds
 *
 * Differences from Beetle:
 *   - CHR_RAM, DRAM, FB live as byte ranges in our flat s_vip_mem
 *     rather than separate `uint16` arrays. We cast through
 *     `chr_dram_u16ptr()` / `chr_chr_u16ptr()` etc. so the inner loop
 *     code is structurally identical.
 *   - Parallax is always enabled (no Beetle ParallaxDisabled setting).
 *   - The 8-bit-per-pixel scratch is a local on the stack rather than
 *     static module data — `vb_vip_draw_block` is called inside the
 *     column-machine loop and each invocation packs into the FB
 *     immediately, so the lifetime is the function call.
 */

#define BGM_AFFINE 0x2u
#define BGM_OBJ    0x3u

static int s_obj_search_which;

static inline const uint16_t* dram_u16(void) {
    return (const uint16_t*)&s_vip_mem[0x20000u];
}
static inline const uint16_t* chr_u16(void) {
    return (const uint16_t*)&s_vip_mem[VIP_CHR_CANONICAL_BASE];
}

static void source_texel(uint16_t* attribution, int eye, unsigned tile,
                         unsigned u, unsigned v, unsigned x, unsigned y, unsigned raw) {
    if (!attribution || !vb_renderer_tracks_texels()) return;
    tile &= 2047;
    if (!s_source_hashed[tile]) {
        s_source_hash[tile] = vb_capture_hash(&chr_u16()[tile * 8]);
        s_source_hashed[tile] = 1;
    }
    VbSourceTexel* out = &s_source_row[eye][attribution - s_attr_row[eye]];
    *out = (VbSourceTexel){s_source_hash[tile], (uint16_t)x, (uint16_t)y,
        (uint16_t)tile, (uint8_t)u, (uint8_t)v, s_source_map, s_source_kind,
        (uint8_t)raw, (uint8_t)s_attr_val};
}

static inline int16_t sign_10(uint32_t v) {
    return (int16_t)(((v & 0x3FFu) ^ 0x200u) - 0x200u);
}
static inline int16_t sign_11(uint32_t v) {
    return (int16_t)(((v & 0x7FFu) ^ 0x400u) - 0x400u);
}
static inline int16_t sign_9(uint32_t v) {
    return (int16_t)(((v & 0x1FFu) ^ 0x100u) - 0x100u);
}
static inline int32_t sign_x_s32(uint32_t v, int x) {
    uint32_t mask = (x >= 32) ? 0xFFFFFFFFu : ((1u << x) - 1u);
    uint32_t sgn  = 1u << (x - 1);
    return (int32_t)(((v & mask) ^ sgn) - sgn);
}

static void draw_bg(uint8_t* target, uint16_t* attr_target, uint16_t real_y, int lr,
                    uint8_t bgmap_base_raw, int overplane,
                    uint16_t overplane_char,
                    int32_t source_x, int32_t source_y,
                    uint32_t scx, uint32_t scy,
                    uint16_t dest_x, uint16_t dest_y,
                    uint16_t dest_width, uint16_t dest_height) {
    (void)lr;
    const uint16_t* chr  = chr_u16();
    const uint16_t* bgm  = dram_u16();
    uint32_t bgmap_base  = (uint32_t)bgmap_base_raw << 12;
    int32_t  start_x, final_x;
    const uint32_t bgsc_overplane = bgm[overplane_char];
    const uint32_t bgmap_xcount  = 1u << scx;
    const uint32_t bgmap_ycount  = 1u << scy;
    const uint32_t source_x_size = 512u * bgmap_xcount;
    const uint32_t source_y_size = 512u * bgmap_ycount;
    const uint32_t source_x_mask = overplane ? 0x1FFFu : (source_x_size - 1u);
    const uint32_t source_y_mask = overplane ? 0x1FFFu : (source_y_size - 1u);

    if ((uint16_t)(real_y - dest_y) > dest_height) return;

    dest_x = (uint16_t)sign_10((uint32_t)dest_x);

    if (dest_x & 0x8000u) source_x -= (int16_t)dest_x;

    start_x = (int16_t)dest_x;
    final_x = (int16_t)dest_x + dest_width;
    if (start_x < 0)   start_x = 0;
    if (final_x > 383) final_x = 383;
    if (start_x > final_x) return;

    source_y &= (int32_t)source_y_mask;
    bgmap_base |= (((uint32_t)source_y >> 3) & 0x3Fu) * 0x40u
                | (((uint32_t)source_y << 3) & ~0xFFFu) << scx;

    for (int x = start_x; x <= final_x; x++) {
        uint32_t source_x_u = (uint32_t)source_x & source_x_mask;
        uint32_t bgsc = bgsc_overplane;

        if (source_x_u < source_x_size && (uint32_t)source_y < source_y_size) {
            uint32_t idx = (bgmap_base
                          | ((source_x_u << 3) & ~0xFFFu)
                          | ((source_x_u >> 3) & 0x3Fu)) & 0xFFFFu;
            bgsc = bgm[idx];
        }

        uint32_t char_no         = bgsc & 0x7FFu;
        uint32_t palette_sel     = bgsc >> 14;
        uint32_t hflip_xor       = (bgsc & 0x2000u) ? 7u : 0u;
        uint32_t vflip_xor       = (bgsc & 0x1000u) ? 7u : 0u;
        uint32_t char_sub_y      = vflip_xor ^ ((uint32_t)source_y & 0x7u);

        uint16_t attr_id = s_attr_val;   /* world+1 (set per world by the caller) */
        if (!(source_x_u & 7u) && (x + 7) <= final_x) {
            uint32_t pixels = chr[char_no * 8u + char_sub_y];
            if (bgsc & 0x2000u) {
                for (int sub = 0; sub < 8; ++sub) {
                    uint32_t v = (pixels >> (14 - sub * 2)) & 3u;
                    if (v) { target[x + sub] = s_gplt_cache[palette_sel][v];
                             if (attr_target) { attr_target[x + sub] = attr_id;
                                 source_texel(attr_target+x+sub, lr, char_no, hflip_xor ^ sub, char_sub_y, source_x_u+sub, (uint32_t)source_y, v); } }
                }
            } else {
                for (int sub = 0; sub < 8; ++sub) {
                    uint32_t v = (pixels >> (sub * 2)) & 3u;
                    if (v) { target[x + sub] = s_gplt_cache[palette_sel][v];
                             if (attr_target) { attr_target[x + sub] = attr_id;
                                 source_texel(attr_target+x+sub, lr, char_no, hflip_xor ^ sub, char_sub_y, source_x_u+sub, (uint32_t)source_y, v); } }
                }
            }
            x += 7;
            source_x += 8;
        } else {
            uint32_t char_sub_x = hflip_xor ^ (source_x_u & 0x7u);
            uint8_t pixel = (uint8_t)((chr[char_no * 8u + char_sub_y] >> (char_sub_x * 2u)) & 0x3u);
            if (pixel) { target[x] = s_gplt_cache[palette_sel][pixel];
                         if (attr_target) { attr_target[x] = attr_id;
                             source_texel(attr_target+x, lr, char_no, char_sub_x, char_sub_y, source_x_u, (uint32_t)source_y, pixel); } }
            source_x++;
        }
    }
}

static void draw_affine(uint8_t* target, uint16_t* attr_target, uint16_t real_y, int lr,
                        uint32_t param_base, uint32_t bgmap_base,
                        int overplane_mode, uint16_t overplane_char,
                        uint32_t scx, uint32_t scy,
                        uint16_t dest_x, uint16_t dest_y,
                        uint16_t dest_width, uint16_t dest_height) {
    const uint16_t* chr = chr_u16();
    const uint16_t* bgm = dram_u16();
    const uint32_t bgmap_xcount  = 1u << scx;
    const uint32_t bgmap_ycount  = 1u << scy;
    const uint32_t source_x_size = 512u * bgmap_xcount;
    const uint32_t source_y_size = 512u * bgmap_ycount;
    const uint16_t* pp = &bgm[(param_base + 8u * (real_y - dest_y)) & 0xFFFFu];
    int16_t mx = (int16_t)pp[0];
    int16_t mp = (int16_t)pp[1];
    int16_t my = (int16_t)pp[2];
    int16_t dx = (int16_t)pp[3];
    int16_t dy = (int16_t)pp[4];
    uint32_t source_x, source_y;
    uint32_t source_x_mask, source_y_mask;
    int32_t  start_x, final_x;
    const uint32_t bgsc_overplane = bgm[overplane_char];

    dest_x = (uint16_t)sign_10((uint32_t)dest_x);
    if ((uint16_t)(real_y - dest_y) > dest_height) return;

    source_x = (uint32_t)((int32_t)mx << 6);
    source_y = (uint32_t)((int32_t)my << 6);

    if (dest_x & 0x8000u) {
        source_x += (uint32_t)((int32_t)dx * (65536 - (int32_t)dest_x));
        source_y += (uint32_t)((int32_t)dy * (65536 - (int32_t)dest_x));
    }
    if (mp >= 0 && lr) {
        source_x += (uint32_t)((int32_t)dx * mp);
        source_y += (uint32_t)((int32_t)dy * mp);
    } else if (mp < 0 && !lr) {
        source_x += (uint32_t)((int32_t)dx * -mp);
        source_y += (uint32_t)((int32_t)dy * -mp);
    }

    if (overplane_mode) {
        source_x_mask = 0x3FFFFFFu;
        source_y_mask = 0x3FFFFFFu;
    } else {
        source_x_mask = (source_x_size << 9) - 1u;
        source_y_mask = (source_y_size << 9) - 1u;
    }

    start_x = (int16_t)dest_x;
    final_x = (int16_t)dest_x + dest_width;
    if (start_x < 0) start_x = 0;
    if (final_x > 383) final_x = 383;

    if (dy == 0) {
        source_y &= source_y_mask;
        if (source_y >= (source_y_size << 9)) return;
        bgmap_base |= (((source_y >> 6) & ~0xFFFu) << scx)
                    | (((source_y >> 12) & 0x3Fu) * 0x40u);
        for (int x = start_x; x <= final_x; x++) {
            uint32_t bgsc = bgsc_overplane;
            source_x &= source_x_mask;
            if (source_x < (source_x_size << 9)) {
                uint32_t idx = (bgmap_base
                              | ((source_x >> 6) & ~0xFFFu)
                              | ((source_x >> 12) & 0x3Fu)) & 0xFFFFu;
                bgsc = bgm[idx];
            }
            uint32_t hflip_xor = (uint32_t)(((int32_t)(bgsc << 18) >> 30) & 0xE);
            uint32_t vflip_xor = (uint32_t)(((int32_t)(bgsc << 19) >> 31) & 0x7);
            uint32_t char_sub_y = vflip_xor ^ ((source_y >> 9) & 0x7u);
            uint32_t char_sub_x = hflip_xor ^ ((source_x >> 8) & 0xEu);
            uint32_t pixel = (chr[((bgsc & 0x7FFu) * 8u) | char_sub_y] >> char_sub_x) & 0x3u;
            if (pixel) { target[x] = s_gplt_cache[bgsc >> 14][pixel];
                         if (attr_target) { attr_target[x] = s_attr_val;
                             source_texel(attr_target+x, lr, bgsc & 2047u, char_sub_x / 2, char_sub_y, source_x >> 9, source_y >> 9, pixel); } }
            source_x = (uint32_t)((int32_t)source_x + dx);
        }
    } else {
        for (int x = start_x; x <= final_x; x++) {
            uint32_t bgsc = bgsc_overplane;
            source_x &= source_x_mask;
            source_y &= source_y_mask;
            if (source_x < (source_x_size << 9) && source_y < (source_y_size << 9)) {
                uint32_t m_index   = ((source_x >> 6) & ~0xFFFu)
                                   + (((source_y >> 6) & ~0xFFFu) << scx);
                uint32_t sub_index = ((source_x >> 12) & 0x3Fu)
                                   + (((source_y >> 12) & 0x3Fu) * 0x40u);
                bgsc = bgm[(bgmap_base | m_index | sub_index) & 0xFFFFu];
            }
            uint32_t char_no    = bgsc & 0x7FFu;
            uint32_t palette    = bgsc >> 14;
            uint32_t hflip_xor  = (bgsc & 0x2000u) ? 7u : 0u;
            uint32_t vflip_xor  = (bgsc & 0x1000u) ? 7u : 0u;
            uint32_t char_sub_y = vflip_xor ^ ((source_y >> 9) & 0x7u);
            uint32_t char_sub_x = hflip_xor ^ ((source_x >> 9) & 0x7u);
            uint8_t pixel = (uint8_t)((chr[char_no * 8u + char_sub_y] >> (char_sub_x * 2u)) & 0x3u);
            if (pixel) { target[x] = s_gplt_cache[palette][pixel];
                         if (attr_target) { attr_target[x] = s_attr_val;
                             source_texel(attr_target+x, lr, char_no, char_sub_x, char_sub_y, source_x >> 9, source_y >> 9, pixel); } }
            source_x = (uint32_t)((int32_t)source_x + dx);
            source_y = (uint32_t)((int32_t)source_y + dy);
        }
    }
}

static void draw_obj(uint8_t* fb_lr[2], uint16_t* attr_lr[2], uint16_t y, int lron[2]) {
    const uint16_t* chr = chr_u16();
    const uint16_t* bgm = dram_u16();
    int32_t start_oam = s_spt[s_obj_search_which];
    int32_t end_oam   = s_obj_search_which
                        ? s_spt[s_obj_search_which - 1] : 1023;
    int32_t oam = start_oam;

    do {
        const uint16_t* oam_ptr = &bgm[(0x1E000u + (uint32_t)oam * 8u) >> 1];

        /* Override suppression (experiment): skip an OBJ whose tile the
         * resolve pass matched to a replacement, so the present-time RGBA
         * overlay stands in. The (char_no+1) re-check fails safe — if the
         * OAM entry's char changed since resolve, draw the original 2bpp.
         * s_obj_suppress is all-zero unless VBRECOMP_OVERRIDES is active, so
         * this branch is never taken on a faithful run. */
        if (s_obj_suppress[oam] &&
            (uint16_t)((oam_ptr[3] & 0x7FFu) + 1u) == s_obj_suppress[oam]) {
            continue;
        }

        uint32_t jy = oam_ptr[2];
        uint32_t tile_y = (y - jy) & 0xFFu;
        if (tile_y >= 8u) continue;

        uint32_t jx = oam_ptr[0];
        uint32_t jp = oam_ptr[1] & 0x3FFFu;
        uint32_t palette_sel = oam_ptr[3] >> 14;
        uint32_t vflip_xor   = (oam_ptr[3] & 0x1000u) ? 7u : 0u;
        uint32_t char_sub_y  = vflip_xor ^ tile_y;
        int      jlron[2]    = { (int)(oam_ptr[1] & 0x8000u),
                                 (int)(oam_ptr[1] & 0x4000u) };
        uint32_t char_no     = oam_ptr[3] & 0x7FFu;
        uint32_t pixels_save = chr[char_no * 8u + char_sub_y];

        for (int lr = 0; lr < 2; lr++) {
            if (!(jlron[lr] && lron[lr])) continue;
            uint32_t pixels = pixels_save;
            int32_t  x = sign_x_s32(jx + (lr ? jp : -jp), 10);
            if (x >= -7 && x < 384) {
                uint8_t*  tgt  = &fb_lr[lr][x];
                uint16_t* atgt = attr_lr[lr] ? &attr_lr[lr][x] : NULL;
                uint16_t  id   = s_attr_val;   /* world+1 (set per world) */
                if (oam_ptr[3] & 0x2000u) {
                    tgt += 7; if (atgt) atgt += 7;
                    for (int m = 8; m; m--) {
                        if (pixels & 3u) { *tgt = s_jplt_cache[palette_sel][pixels & 3u];
                                           if (atgt) { *atgt = id;
                                               source_texel(atgt, lr, char_no, 8-m, char_sub_y, 8-m, char_sub_y, pixels & 3u); } }
                        tgt--; if (atgt) atgt--;
                        pixels >>= 2;
                    }
                } else {
                    for (int m = 8; m; m--) {
                        if (pixels & 3u) { *tgt = s_jplt_cache[palette_sel][pixels & 3u];
                                           if (atgt) { *atgt = id;
                                               source_texel(atgt, lr, char_no, 8-m, char_sub_y, 8-m, char_sub_y, pixels & 3u); } }
                        tgt++; if (atgt) atgt++;
                        pixels >>= 2;
                    }
                }
            }
        }
    } while ((oam = (oam - 1) & 1023) != end_oam);
}

/* Render one 8-row block (block 0..27) into the back-buffer FBs. */
static void vip_draw_block_into(uint8_t block_no,
                                uint8_t* fb_l, uint8_t* fb_r) {
    /* Per-row temp buffer, 512 bytes wide so OBJ pixels with x ∈
     * [-7, 384) land safely. The +8 left-pad mirrors Beetle's
     * `DrawingBuffers[lr][8 + x + 512 * row]`. */
    const int ROW_STRIDE = 512;
    const int PAD_LEFT   = 8;
    static uint8_t  s_row_buf[2][8 * 512];
    memset(s_source_hashed, 0, sizeof(s_source_hashed));
    if (vb_renderer_tracks_texels()) memset(s_source_row, 0, sizeof(s_source_row));
    const uint16_t* bgm = dram_u16();

    uint8_t bkcol = (uint8_t)(s_bkcol & 0x3u);
    for (int y = 0; y < 8; y++) {
        memset(&s_row_buf[0][y * ROW_STRIDE], bkcol, ROW_STRIDE);
        memset(&s_row_buf[1][y * ROW_STRIDE], bkcol, ROW_STRIDE);
    }
    if (s_attr_on) {
        memset(s_attr_row[0], 0, sizeof(s_attr_row[0]));
        memset(s_attr_row[1], 0, sizeof(s_attr_row[1]));
    }

    s_obj_search_which = 3;

    for (int world = 31; world >= 0; world--) {
        const uint16_t* wp = &bgm[(0x1D800u + (uint32_t)world * 0x20u) >> 1];

        uint32_t bgmap_base    = wp[0] & 0xFu;
        int      end           = (wp[0] & 0x40u) != 0;
        int      over          = (wp[0] & 0x80u) != 0;
        uint32_t scy           = (wp[0] >> 8) & 3u;
        uint32_t scx           = (wp[0] >> 10) & 3u;
        uint32_t bgm_mode      = (wp[0] >> 12) & 3u;
        int      lron[2]       = { (int)(wp[0] & 0x8000u),
                                   (int)(wp[0] & 0x4000u) };
        uint16_t gx            = (uint16_t)sign_11(wp[1]);
        uint16_t gp            = (uint16_t)sign_9(wp[2]);
        uint16_t gy            = (uint16_t)sign_11(wp[3]);
        uint16_t mx            = wp[4];
        uint16_t mp            = (uint16_t)sign_9(wp[5]);
        uint16_t my            = wp[6];
        uint16_t window_w      = (uint16_t)sign_11(wp[7]);
        uint16_t window_h      = wp[8] & 0x3FFu;
        uint32_t param_base    = wp[9] & 0xFFF0u;
        uint16_t overplane_chr = wp[10];

        if (end) break;

        s_source_map = bgm_mode == BGM_OBJ ? 255 : (uint8_t)bgmap_base;
        s_source_kind = (uint8_t)bgm_mode;
        s_attr_val = (uint16_t)(world + 1);  /* recolor attribution for this world */

        for (int y = 0; y < 8; y++) {
            uint8_t* fb[2] = {
                &s_row_buf[0][PAD_LEFT + y * ROW_STRIDE],
                &s_row_buf[1][PAD_LEFT + y * ROW_STRIDE],
            };
            uint16_t* af[2] = {
                s_attr_on ? &s_attr_row[0][PAD_LEFT + y * ROW_STRIDE] : NULL,
                s_attr_on ? &s_attr_row[1][PAD_LEFT + y * ROW_STRIDE] : NULL,
            };

            if (bgm_mode == BGM_OBJ) {
                draw_obj(fb, af, (uint16_t)(block_no * 8 + y), lron);
            } else if (bgm_mode == BGM_AFFINE) {
                for (int lr = 0; lr < 2; lr++) {
                    if (lron[lr]) {
                        draw_affine(fb[lr], af[lr],
                                    (uint16_t)(block_no * 8 + y), lr,
                                    param_base, bgmap_base * 4096u,
                                    over, overplane_chr,
                                    scx, scy,
                                    gx + (uint16_t)(lr ? gp : -gp),
                                    gy, window_w, window_h);
                    }
                }
            } else {
                for (int lr = 0; lr < 2; lr++) {
                    uint16_t real_y = (uint16_t)(block_no * 8 + y);
                    uint16_t src_x  = mx + (uint16_t)(lr ? mp : -mp);
                    uint16_t src_y  = my + (uint16_t)(real_y - gy);
                    uint16_t dst_x  = gx + (uint16_t)(lr ? gp : -gp);
                    uint16_t dst_y  = gy;

                    if (!lron[lr]) continue;
                    if (bgm_mode == 1u) {
                        src_x += (uint16_t)(int16_t)bgm[
                            (param_base + (((real_y - dst_y) * 2u) | (uint32_t)lr))
                            & 0xFFFFu];
                    }
                    draw_bg(fb[lr], af[lr], real_y, lr, (uint8_t)bgmap_base,
                            over, overplane_chr,
                            (int32_t)(int16_t)src_x,
                            (int32_t)(int16_t)src_y,
                            scx, scy, dst_x, dst_y, window_w, window_h);
                }
            }
        }

        if (bgm_mode == BGM_OBJ && s_obj_search_which) s_obj_search_which--;
    }

    /* Pack the 8bpp scratch into 2bpp framebuffer columns. Each
     * column gets 2 bytes for this block: byte 0 = rows 0..3 (2bpp
     * each, low-bit-first), byte 1 = rows 4..7. */
    for (int lr = 0; lr < 2; lr++) {
        uint8_t* fb_target = (lr == 0 ? fb_l : fb_r) + block_no * 2;
        const uint8_t* src = s_row_buf[lr];
        for (int x = 0; x < 384; x++) {
            uint8_t b0 =
                  (uint8_t)((src[PAD_LEFT + x + 512 * 0] & 3u) << 0)
                | (uint8_t)((src[PAD_LEFT + x + 512 * 1] & 3u) << 2)
                | (uint8_t)((src[PAD_LEFT + x + 512 * 2] & 3u) << 4)
                | (uint8_t)((src[PAD_LEFT + x + 512 * 3] & 3u) << 6);
            uint8_t b1 =
                  (uint8_t)((src[PAD_LEFT + x + 512 * 4] & 3u) << 0)
                | (uint8_t)((src[PAD_LEFT + x + 512 * 5] & 3u) << 2)
                | (uint8_t)((src[PAD_LEFT + x + 512 * 6] & 3u) << 4)
                | (uint8_t)((src[PAD_LEFT + x + 512 * 7] & 3u) << 6);
            fb_target[64 * x + 0] = b0;
            fb_target[64 * x + 1] = b1;
        }
    }

    /* Pack the parallel attribution scratch into the full-res per-eye
     * attribution buffer for the current drawing FB slot, so the present
     * recolor path can map each displayed pixel back to its source tile. */
    if (s_attr_on) {
        for (int lr = 0; lr < 2; lr++) {
            const uint16_t* asrc = s_attr_row[lr];
            uint16_t* adst = s_attr_fb[s_drawing_fb & 1][lr];
            for (int row = 0; row < 8; row++) {
                int yy = block_no * 8 + row;
                for (int x = 0; x < 384; x++)
                {
                    adst[yy * 384 + x] = asrc[PAD_LEFT + x + 512 * row];
                    if (vb_renderer_tracks_texels())
                        s_source_fb[s_drawing_fb & 1][lr][yy * 384 + x] =
                            s_source_row[lr][PAD_LEFT + x + 512 * row];
                }
            }
        }
    }
}

/* Public entry point — called by the column state machine when the
 * 1120*4 cycle drawing-block timer fires. Resolves the drawing-FB
 * base addresses in s_vip_mem and drives vip_draw_block_into. */
static void vip_draw_block_now(uint8_t block_no) {
    /* L drawing FB base = (drawing_fb==0) ? 0x0000 : 0x8000
     * R drawing FB base = (drawing_fb==0) ? 0x10000 : 0x18000 */
    uint8_t* fb_l = &s_vip_mem[s_drawing_fb ? 0x8000u  : 0x0000u];
    uint8_t* fb_r = &s_vip_mem[s_drawing_fb ? 0x18000u : 0x10000u];
    vip_draw_block_into(block_no, fb_l, fb_r);
}


/* ----------------- Capture pass (experiment, opt-in) ----------------- *
 *
 * Walks the world descriptors 31..0 exactly as vip_draw_block_into does,
 * recording — for each OBJ — the full 8x8 CHR tile (by content hash) plus a
 * draw-use with per-eye placement/parallax/visibility/flip/priority. BG and
 * affine worlds are recorded at world granularity (position + visibility);
 * per-cell BG tile enumeration is a documented follow-up. Read-only over the
 * VIP DRAM/CHR shadow — never writes a framebuffer. */
static void vip_capture_pass(void) {
    const uint16_t* bgm = dram_u16();
    const uint16_t* chr = chr_u16();

    vb_capture_begin_frame();

    int which = 3;   /* mirrors s_obj_search_which in the renderer */

    for (int world = 31; world >= 0; world--) {
        const uint16_t* wp = &bgm[(0x1D800u + (uint32_t)world * 0x20u) >> 1];

        uint32_t bgm_mode = (wp[0] >> 12) & 3u;
        int      end      = (wp[0] & 0x40u) != 0;
        int      lron[2]  = { (int)(wp[0] & 0x8000u), (int)(wp[0] & 0x4000u) };

        if (end) break;

        if (bgm_mode == BGM_OBJ) {
            int32_t start_oam = s_spt[which];
            int32_t end_oam   = which ? s_spt[which - 1] : 1023;
            int32_t oam = start_oam;
            do {
                const uint16_t* oam_ptr = &bgm[(0x1E000u + (uint32_t)oam * 8u) >> 1];
                uint32_t jx          = oam_ptr[0];
                uint32_t jp          = oam_ptr[1] & 0x3FFFu;
                int      jlron[2]    = { (int)(oam_ptr[1] & 0x8000u),
                                         (int)(oam_ptr[1] & 0x4000u) };
                uint32_t jy          = oam_ptr[2];
                uint32_t char_no     = oam_ptr[3] & 0x7FFu;
                uint32_t hflip       = (oam_ptr[3] & 0x2000u) ? 1u : 0u;
                uint32_t vflip       = (oam_ptr[3] & 0x1000u) ? 1u : 0u;
                uint32_t palette     = oam_ptr[3] >> 14;

                int vis_l = (jlron[0] && lron[0]) ? 1 : 0;
                int vis_r = (jlron[1] && lron[1]) ? 1 : 0;
                if (vis_l || vis_r) {
                    uint16_t tile[8];
                    for (int k = 0; k < 8; ++k) tile[k] = chr[char_no * 8u + k];
                    uint32_t h = vb_capture_tile(tile, VB_CAP_CTX_OBJ,
                                                 (uint8_t)palette);
                    VbCaptureUse u;
                    u.tile_hash = h;
                    u.world_idx = (uint16_t)world;
                    u.oam_idx   = (uint16_t)oam;
                    u.x_l = (int16_t)sign_x_s32(jx - jp, 10);
                    u.x_r = (int16_t)sign_x_s32(jx + jp, 10);
                    u.y   = (int16_t)jy;
                    u.ctx = VB_CAP_CTX_OBJ;
                    u.vis_l = (uint8_t)vis_l;
                    u.vis_r = (uint8_t)vis_r;
                    u.hflip = (uint8_t)hflip;
                    u.vflip = (uint8_t)vflip;
                    u.palette = (uint8_t)palette;
                    vb_capture_use(&u);
                }
            } while ((oam = (oam - 1) & 1023) != end_oam);

            if (which) which--;
        } else {
            /* BG (mode 0/1) or affine (mode 2): world-level record. */
            uint16_t gx = (uint16_t)sign_11(wp[1]);
            uint16_t gp = (uint16_t)sign_9(wp[2]);
            uint16_t gy = (uint16_t)sign_11(wp[3]);
            VbCaptureUse u;
            memset(&u, 0, sizeof(u));
            u.tile_hash = 0;
            u.world_idx = (uint16_t)world;
            u.oam_idx   = 0xFFFF;
            u.x_l = (int16_t)(int16_t)(gx - gp);
            u.x_r = (int16_t)(int16_t)(gx + gp);
            u.y   = (int16_t)gy;
            u.ctx = (bgm_mode == BGM_AFFINE) ? VB_CAP_CTX_AFFINE : VB_CAP_CTX_BG;
            u.vis_l = lron[0] ? 1 : 0;
            u.vis_r = lron[1] ? 1 : 0;
            vb_capture_use(&u);
        }
    }

    vb_capture_end_frame();
}


/* ----------------- Override resolve pass (experiment, opt-in) ----------- *
 *
 * Same OBJ walk as the capture pass, but matches each OBJ's unflipped tile
 * content hash against the loaded override pack. On a match it (a) records
 * (char_no+1) in s_obj_suppress[oam] so draw_obj skips the original 2bpp,
 * and (b) queues a per-eye overlay blit (image + per-eye X + flips +
 * visibility) into the draw-list slot for the frame being drawn
 * (s_drawing_fb) — consumed at present when that buffer is displayed. */
static void vip_resolve_overrides(void) {
    const uint16_t* bgm = dram_u16();
    const uint16_t* chr = chr_u16();
    int slot = s_drawing_fb & 1;

    memset(s_obj_suppress, 0, sizeof(s_obj_suppress));
    vb_overlay_reset(slot);

    int which = 3;
    for (int world = 31; world >= 0; world--) {
        const uint16_t* wp = &bgm[(0x1D800u + (uint32_t)world * 0x20u) >> 1];
        uint32_t bgm_mode = (wp[0] >> 12) & 3u;
        int      end      = (wp[0] & 0x40u) != 0;
        int      lron[2]  = { (int)(wp[0] & 0x8000u), (int)(wp[0] & 0x4000u) };
        if (end) break;
        if (bgm_mode != BGM_OBJ) continue;

        int32_t start_oam = s_spt[which];
        int32_t end_oam   = which ? s_spt[which - 1] : 1023;
        int32_t oam = start_oam;
        do {
            const uint16_t* oam_ptr = &bgm[(0x1E000u + (uint32_t)oam * 8u) >> 1];
            uint32_t char_no = oam_ptr[3] & 0x7FFu;
            uint16_t tile[8];
            for (int k = 0; k < 8; ++k) tile[k] = chr[char_no * 8u + k];
            const VbOverrideImage* img = vb_overrides_lookup_tile(vb_capture_hash(tile));
            if (!img) continue;

            int jlron0 = (int)(oam_ptr[1] & 0x8000u);
            int jlron1 = (int)(oam_ptr[1] & 0x4000u);
            int vis_l = (jlron0 && lron[0]) ? 1 : 0;
            int vis_r = (jlron1 && lron[1]) ? 1 : 0;
            if (!vis_l && !vis_r) continue;

            uint32_t jx = oam_ptr[0];
            uint32_t jp = oam_ptr[1] & 0x3FFFu;
            VbOverlayCmd cmd;
            cmd.img   = img;
            cmd.x_l   = (int16_t)sign_x_s32(jx - jp, 10);
            cmd.x_r   = (int16_t)sign_x_s32(jx + jp, 10);
            cmd.y     = (int16_t)oam_ptr[2];
            cmd.hflip = (oam_ptr[3] & 0x2000u) ? 1 : 0;
            cmd.vflip = (oam_ptr[3] & 0x1000u) ? 1 : 0;
            cmd.vis_l = (uint8_t)vis_l;
            cmd.vis_r = (uint8_t)vis_r;
            vb_overlay_add(slot, &cmd);

            s_obj_suppress[oam] = (uint16_t)(char_no + 1u);
        } while ((oam = (oam - 1) & 1023) != end_oam);

        if (which) which--;
    }
}


/* Convert the display-FB to ARGB8888 for screenshot / SDL output.
 *
 * The 2bpp framebuffer stores 4 pixels per byte in the column-major
 * arrangement documented at the renderer top. We walk pixels in the
 * natural (x, y) order and unpack on the fly. */
void vb_vip_render_framebuffer(int eye, uint32_t* argb_out) {
    if (!argb_out) return;
    uint32_t base;
    if (eye == 0) {
        base = s_display_fb ? 0x8000u  : 0x0000u;   /* L */
    } else {
        base = s_display_fb ? 0x18000u : 0x10000u;  /* R */
    }
    const uint8_t* fb = &s_vip_mem[base];
    for (int y = 0; y < 224; y++) {
        int block      = y >> 3;            /* 0..27 */
        int row_in_blk = y & 7;             /* 0..7 */
        int byte_off   = block * 2 + (row_in_blk >> 2);
        int bit_shift  = (row_in_blk & 3) * 2;
        for (int x = 0; x < 384; x++) {
            uint8_t b = fb[x * 64 + byte_off];
            uint32_t v = (b >> bit_shift) & 3u;
            int32_t bv = s_brt_cache[v];
            if (bv < 0) bv = 0;
            if (bv > 255) bv = 255;
            /* Beetle MakeColorLUT (vip.c:108-144) applies a 1/2.2
             * gamma curve before emitting the host pixel. Skipping it
             * gives BrightnessCache[3]=199 → R=0xC7 instead of 0xE3. */
            uint32_t r = s_color_lut[bv];
            /* Virtual Boy LEDs are red-only; G=B=0 to match what the
             * real hardware emits (and what the Beetle oracle outputs
             * via libretro's XRGB8888 framebuffer).
             *
             * Present-time only: route the gamma-corrected red value
             * through the opt-in red-LED present LUT. Default (RAW) is
             * an exact passthrough — `vb_red_lut_map(r)` returns
             * `0xFF000000 | (r<<16)`, so the verified/oracle-compared
             * frame is byte-identical unless VBRECOMP_SCREEN opts in.
             * This never feeds the verify path (raw `r` stays the
             * oracle); see red_lut.h. */
            argb_out[y * 384 + x] = vb_red_lut_map((int)r);
        }
    }
}

/* Present-time full-screen recolor (experiment, opt-in). Same FB unpack as
 * vb_vip_render_framebuffer, but each pixel is mapped through the world-based
 * recolor pack: per displayed frame we compute each world's on-screen vertical
 * extent, then color each pixel by (world, position within that world, value).
 * Pixels whose world has no pack rule (or background, attr 0) fall back to the
 * faithful red. Kept separate so vb_vip_render_framebuffer stays
 * byte-identical / oracle-safe. */
void vb_vip_render_framebuffer_recolored(int eye, uint32_t* argb_out) {
    if (!argb_out) return;
    uint32_t base = (eye == 0) ? (s_display_fb ? 0x8000u  : 0x0000u)
                               : (s_display_fb ? 0x18000u : 0x10000u);
    const uint8_t*  fb   = &s_vip_mem[base];
    const uint16_t* attr = s_attr_fb[s_display_fb & 1][eye ? 1 : 0];

    /* Pre-pass: per-world (attr value 1..32) on-screen bounding box + count.
     * Both axes: vertical (wy0/wy1) feeds the recolor band model, horizontal
     * (wx0/wx1) feeds its column model (a row of distinct sprites drawn under
     * one world index, e.g. a character roster). */
    int wy0[33], wy1[33], wx0[33], wx1[33], wcnt[33];
    for (int i = 0; i < 33; ++i) {
        wy0[i] = wx0[i] = 1 << 20; wy1[i] = wx1[i] = -1; wcnt[i] = 0;
    }
    for (int y = 0; y < 224; ++y) {
        const uint16_t* row = &attr[y * 384];
        for (int x = 0; x < 384; ++x) {
            uint16_t a = row[x];
            if (a == 0 || a > 32) continue;
            if (y < wy0[a]) wy0[a] = y;
            if (y > wy1[a]) wy1[a] = y;
            if (x < wx0[a]) wx0[a] = x;
            if (x > wx1[a]) wx1[a] = x;
            wcnt[a]++;
        }
    }

    /* Select the active scene from the set of worlds present this frame. A
     * world counts as "present" only above a small pixel threshold so a few
     * stray attributed pixels don't flip the detected scene. attr value a
     * encodes world index a-1, so bit (a-1) of the mask. */
    uint32_t active_mask = 0;
    for (int a = 1; a <= 32; ++a)
        if (wcnt[a] >= 16) active_mask |= (1u << (a - 1));
    vb_recolor_select_scene(active_mask);

    /* Feed the always-on observation rings (deduped per displayed frame, so the
     * eye-1 and screenshot calls here don't double-record). */
    vip_rings_record(active_mask, wy0, wy1, wx0, wx1, wcnt);

    for (int y = 0; y < 224; y++) {
        int block      = y >> 3;
        int row_in_blk = y & 7;
        int byte_off   = block * 2 + (row_in_blk >> 2);
        int bit_shift  = (row_in_blk & 3) * 2;
        for (int x = 0; x < 384; x++) {
            uint8_t  b  = fb[x * 64 + byte_off];
            uint32_t v  = (b >> bit_shift) & 3u;
            uint16_t a  = attr[y * 384 + x];
            uint32_t out;
            if (a != 0 && a <= 32 && wy1[a] >= 0 &&
                vb_recolor_world_pixel((int)a - 1,
                                       x - wx0[a], wx1[a] - wx0[a] + 1,
                                       y - wy0[a], wy1[a] - wy0[a] + 1,
                                       (int)v, &out)) {
                argb_out[y * 384 + x] = out;
            } else {
                int32_t bv = s_brt_cache[v];
                if (bv < 0) bv = 0;
                if (bv > 255) bv = 255;
                argb_out[y * 384 + x] = vb_red_lut_map((int)s_color_lut[bv]);
            }
        }
    }
}

int32_t vb_vip_brightness(int v) {
    if (v < 0 || v > 3) return 0;
    return s_brt_cache[v];
}


/* ----------------- Introspection ----------------- */

const uint8_t* vb_vip_shadow(void)      { return s_vip_mem; }
size_t         vb_vip_shadow_size(void) { return sizeof(s_vip_mem); }

uint16_t vb_vip_intpnd(void) { return s_intpnd; }
uint16_t vb_vip_intenb(void) { return s_intenb; }
uint16_t vb_vip_dpctrl(void) { return s_dpctrl; }
/* Reuse the live-read path so DPSTTS/XPSTTS reported via vip_state
 * matches what the recompiled cart sees. The full bus address folds
 * via vip_offset(); the helper expects an offset within the window. */
uint16_t vb_vip_dpstts(void) { return vip_read_register16(vip_offset(VB_VIP_DPSTTS)); }
uint16_t vb_vip_xpctrl(void) { return s_xpctrl; }
uint16_t vb_vip_xpstts(void) { return vip_read_register16(vip_offset(VB_VIP_XPSTTS)); }
uint16_t vb_vip_frmcyc(void) { return s_frmcyc; }
uint16_t vb_vip_bkcol(void)  { return s_bkcol; }
uint16_t vb_vip_brta(void)   { return (uint16_t)s_brta; }
uint16_t vb_vip_brtb(void)   { return (uint16_t)s_brtb; }
uint16_t vb_vip_brtc(void)   { return (uint16_t)s_brtc; }
uint16_t vb_vip_rest(void)   { return (uint16_t)s_rest; }
int32_t  vb_vip_column(void)            { return s_column; }
int32_t  vb_vip_column_counter(void)    { return s_column_counter; }
int32_t  vb_vip_display_region(void)    { return s_display_region; }
int32_t  vb_vip_game_frame_counter(void){ return s_game_frame_counter; }
int32_t  vb_vip_drawing_block(void)     { return s_drawing_block; }
int32_t  vb_vip_drawing_counter(void)   { return s_drawing_counter; }
int      vb_vip_drawing_active(void)    { return s_drawing_active; }
int      vb_vip_display_active(void)    { return s_display_active; }
int      vb_vip_display_fb(void)        { return s_display_fb; }
int      vb_vip_drawing_fb(void)        { return s_drawing_fb; }
uint64_t vb_vip_cycles(void)            { return s_vip_cycles; }

/* Recolor-identification introspection: the displayed eye's attribution buffer
 * (per-pixel char_no|palette<<11; 0 = none) and the content hash of a CHR slot.
 * Used by the `attr_hashes` debug command to list on-screen tiles. Attribution
 * is populated only when capture or recolor is active. */
const uint16_t* vb_vip_attr_buffer(int eye) {
    return s_attr_fb[s_display_fb & 1][eye ? 1 : 0];
}
void vb_vip_copy_levels(int eye, uint8_t* levels) {
    if (!levels) return;
    const unsigned base = (eye ? 0x10000u : 0u) + (s_display_fb ? 0x8000u : 0u);
    const uint8_t* fb = &s_vip_mem[base];
    for (int y = 0; y < 224; ++y)
        for (int x = 0; x < 384; ++x)
            levels[y * 384 + x] = (uint8_t)((fb[x * 64 + y / 4] >> ((y & 3) * 2)) & 3);
}
uint32_t vb_vip_char_hash(uint32_t char_no) {
    return vb_capture_hash(&chr_u16()[(char_no & 0x7FFu) * 8u]);
}

/* ---- Observation-ring introspection (world_trace / wram_anchors) ---- */
uint32_t vb_vip_frame_seq(void) { return s_vip_frame_seq; }

int vb_vip_wring_len(void) { return (int)s_wring_count; }

/* Copy world-ring frame i (0=oldest .. len-1=newest): sets *seq, *mask and
 * writes up to `max` boxes into out[]; returns the box count, or -1 if OOR. */
int vb_vip_wring_get(int i, uint32_t* seq, uint32_t* mask, VbWorldBox* out, int max) {
    int len = (int)s_wring_count;
    if (i < 0 || i >= len) return -1;
    uint32_t base = (s_wring_count < WRING_FRAMES) ? 0 : s_wring_head;
    const WRingFrame* f = &s_wring[(base + (uint32_t)i) % WRING_FRAMES];
    if (seq) *seq = f->seq;
    if (mask) *mask = f->mask;
    int n = f->nbox; if (n > max) n = max;
    for (int k = 0; k < n; ++k) {
        out[k].world = f->box[k].world;
        out[k].x0 = f->box[k].x0; out[k].y0 = f->box[k].y0;
        out[k].x1 = f->box[k].x1; out[k].y1 = f->box[k].y1;
        out[k].count = f->box[k].count;
    }
    return n;
}

int      vb_vip_aring_len(void)    { return (int)s_aring_count; }
uint32_t vb_vip_aring_base(void)   { return ARING_BASE; }
uint32_t vb_vip_aring_window(void) { return ARING_LEN; }

/* Copy WRAM anchor i's window slice [off, off+len): sets *seq, *mask, writes
 * bytes into out; returns bytes copied, or -1 if i out of range / slice OOB. */
int vb_vip_aring_get(int i, uint32_t off, int len, uint32_t* seq, uint32_t* mask,
                     uint8_t* out) {
    int n = (int)s_aring_count;
    if (i < 0 || i >= n) return -1;
    if (len < 0 || off + (uint32_t)len > ARING_LEN) return -1;
    uint32_t base = (s_aring_count < ARING_N) ? 0 : s_aring_head;
    const ARingFrame* f = &s_aring[(base + (uint32_t)i) % ARING_N];
    if (seq) *seq = f->seq;
    if (mask) *mask = f->mask;
    for (int k = 0; k < len; ++k) out[k] = f->buf[off + k];
    return len;
}

const VbSourceTexel* vb_vip_source_buffer(int eye) {
    return s_source_fb[s_display_fb & 1][eye != 0];
}
