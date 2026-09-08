/* vip.h — Virtual Image Processor (display controller) surface.
 *
 * P4-B introduces the column-driven state machine alongside the
 * existing flat 512 KB shadow. The shadow continues to back CHR RAM,
 * BG/OBJ/WORLD DRAM, and the L/R framebuffers (no change to the
 * recompiled cart's load/store paths). The register page at
 * 0x0005F800..0x0005F8FF is now intercepted: reads compute live
 * register values from the state-machine; writes drive control bits
 * (DPCTRL/XPCTRL/INTENB/INTCLR/BRT*) and may clear or assert IRQs.
 *
 * Behavioural oracle: beetle-vb/mednafen/vb/vip.c
 * (VIP_Update / VIP_Power / Read|Write Register).
 */
#ifndef VB_VIP_H
#define VB_VIP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Selected VIP registers — page 0x0005F800. */
#define VB_VIP_INTPND  0x0005F800u   /* interrupt pending */
#define VB_VIP_INTENB  0x0005F802u   /* interrupt enable */
#define VB_VIP_INTCLR  0x0005F804u   /* interrupt clear (W) */
#define VB_VIP_DPSTTS  0x0005F820u   /* display status */
#define VB_VIP_DPCTRL  0x0005F822u   /* display control */
#define VB_VIP_BRTA    0x0005F824u
#define VB_VIP_BRTB    0x0005F826u
#define VB_VIP_BRTC    0x0005F828u
#define VB_VIP_REST    0x0005F82Au
#define VB_VIP_FRMCYC  0x0005F82Eu
#define VB_VIP_CTA     0x0005F830u
#define VB_VIP_XPSTTS  0x0005F840u   /* drawing status */
#define VB_VIP_XPCTRL  0x0005F842u   /* drawing control */
#define VB_VIP_VER     0x0005F844u
#define VB_VIP_SPT0    0x0005F848u   /* SPT0..SPT3 0x848,0x84A,0x84C,0x84E */
#define VB_VIP_GPLT0   0x0005F860u   /* GPLT0..GPLT3 */
#define VB_VIP_JPLT0   0x0005F868u   /* JPLT0..JPLT3 */
#define VB_VIP_BKCOL   0x0005F870u

/* INTPND / INTENB bits (Beetle vip.c:64-72). */
#define VB_VIP_INT_SCANERR    0x0001u
#define VB_VIP_INT_LFB_END    0x0002u
#define VB_VIP_INT_RFB_END    0x0004u
#define VB_VIP_INT_GAME_START 0x0008u
#define VB_VIP_INT_FRAME_START 0x0010u
#define VB_VIP_INT_SB_HIT     0x2000u
#define VB_VIP_INT_XP_END     0x4000u
#define VB_VIP_INT_TIME_ERR   0x8000u
#define VB_VIP_INT_MASK_VALID 0xE01Fu   /* writable bits in INTENB / INTCLR */

void vb_vip_init(void);
void vb_vip_shutdown(void);

/* Advance the VIP state machine by `cycles` V810 cycles. May latch
 * INT_* bits into INTPND and re-assert VBIRQ_SOURCE_VIP. */
void vb_vip_tick(uint64_t cycles);

/* Cycles until the VIP's next state-machine boundary (column/drawing) —
 * the next point it could raise an INTPND event. Used by the event-driven
 * idle loop for precise IRQ-take timing. Always >= 1. */
int32_t vb_vip_cycles_to_next_event(void);

uint8_t  vb_vip_read8 (uint32_t addr);
uint16_t vb_vip_read16(uint32_t addr);
uint32_t vb_vip_read32(uint32_t addr);
void     vb_vip_write8 (uint32_t addr, uint8_t  v);
void     vb_vip_write16(uint32_t addr, uint16_t v);
void     vb_vip_write32(uint32_t addr, uint32_t v);

/* Render the current display framebuffer (display_fb) for one eye into
 * an ARGB8888 output buffer. Buffer must hold 384*224 uint32_t.
 *
 *   eye = 0 → left, eye = 1 → right.
 *
 * The 2bpp framebuffer pixels are mapped through the BRT brightness
 * cache (Beetle vip.c:172-223 RecalcBrightnessCache) to 0..255 grey,
 * then expanded to white-on-black ARGB. This matches Beetle's default
 * (white) color setting; the red-LED color of the real hardware can
 * be layered as a P5 cosmetic flag. */
void vb_vip_render_framebuffer(int eye, uint32_t* argb_out);

/* Opt-in present-time full-screen recolor (experiment): same as
 * vb_vip_render_framebuffer but maps each pixel through the per-frame recolor
 * LUT (see recolor.h). Faithful red for pixels with no pack entry. Used by the
 * present loop / screenshot only when the recolor pack is active; the function
 * above stays the byte-identical oracle path. */
void vb_vip_render_framebuffer_recolored(int eye, uint32_t* argb_out);

/* Returns the current brightness cache value (0..255) for 2bpp
 * framebuffer pixel value 0..3. */
int32_t vb_vip_brightness(int v);

/* Debug introspection (TCP vip_state command). */
const uint8_t* vb_vip_shadow(void);
size_t         vb_vip_shadow_size(void);
uint16_t vb_vip_intpnd(void);
uint16_t vb_vip_intenb(void);
uint16_t vb_vip_dpctrl(void);
uint16_t vb_vip_dpstts(void);
uint16_t vb_vip_xpctrl(void);
uint16_t vb_vip_xpstts(void);
uint16_t vb_vip_frmcyc(void);
uint16_t vb_vip_bkcol(void);
uint16_t vb_vip_brta(void);
uint16_t vb_vip_brtb(void);
uint16_t vb_vip_brtc(void);
uint16_t vb_vip_rest(void);
int32_t  vb_vip_column(void);
int32_t  vb_vip_column_counter(void);
int32_t  vb_vip_display_region(void);
int32_t  vb_vip_game_frame_counter(void);
int32_t  vb_vip_drawing_block(void);
int32_t  vb_vip_drawing_counter(void);
int      vb_vip_drawing_active(void);
int      vb_vip_display_active(void);
int      vb_vip_display_fb(void);
int      vb_vip_drawing_fb(void);
uint64_t vb_vip_cycles(void);

/* Recolor-identification introspection (experiment): the displayed eye's
 * attribution buffer (384*224 world+1 values; 0 = none) and the content
 * hash of a CHR slot. Populated when capture/recolor or a game renderer requests it. */
const uint16_t* vb_vip_attr_buffer(int eye);
void vb_vip_copy_levels(int eye, uint8_t* levels);
uint32_t        vb_vip_char_hash(uint32_t char_no);

/* Always-on observation rings (experiment / collaborative capture). Sized to
 * span a whole play session and walked after the fact — never a short window a
 * probe must race. Both keyed by a per-displayed-frame sequence number, filled
 * only while the recolor render runs (same precondition as world_map).
 *   - World ring: per displayed frame, the active-world mask + each present
 *     world's on-screen bbox (~11 min deep).
 *   - WRAM anchor ring: a game-state WRAM-window snapshot taken at each
 *     world-set transition (one entry per distinct on-screen state). Diff the
 *     window across anchors with different masks to find a sub-state byte.
 * Index i runs 0 = oldest available .. len-1 = newest. */
typedef struct { uint8_t world; int16_t x0, y0, x1, y1; uint32_t count; } VbWorldBox;
uint32_t vb_vip_frame_seq(void);
int      vb_vip_wring_len(void);
int      vb_vip_wring_get(int i, uint32_t* seq, uint32_t* mask, VbWorldBox* out, int max);
int      vb_vip_aring_len(void);
uint32_t vb_vip_aring_base(void);
uint32_t vb_vip_aring_window(void);
int      vb_vip_aring_get(int i, uint32_t off, int len, uint32_t* seq, uint32_t* mask,
                          uint8_t* out);

#ifdef __cplusplus
}
#endif

#endif /* VB_VIP_H */
