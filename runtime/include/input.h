/* input.h — Virtual Boy pad register surface.
 *
 * 16-bit pad register at 0x02000028. Bit layout per docs/HARDWARE_NOTES.md.
 * The Phase 1 skeleton always returns "no buttons pressed"; Phase 4
 * wires SDL keyboard events through `vb_input_set_pad()`.
 */
#ifndef VB_INPUT_H
#define VB_INPUT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VB_PAD_REG     0x02000028u

/* Real-hardware Virtual Boy pad register bit layout. The cart reads
 * SDR_LO/SDR_HI at 0x02000010/0x02000014 and tests specific bits; the
 * layout below mirrors Beetle's VBINPUT_Frame composition:
 *   PadData = (libretro_pad << 2) | 0x2 | battery_low_bit
 * which puts the 14 actual buttons at bits 2..15 and reserves
 *   bit 0 = battery-low flag (0 = OK)
 *   bit 1 = "always 1" / device-connected sentinel
 * The cart's input-polling code tests bit 1 as part of "is controller
 * present", so input.c keeps it forced to 1 on every set_pad call. */
#define VB_PAD_BAT_LOW (1u << 0)   /* status, not a button */
#define VB_PAD_PRESENT (1u << 1)   /* sentinel, always 1 when a pad is "live" */

#define VB_PAD_A       (1u << 2)
#define VB_PAD_B       (1u << 3)
#define VB_PAD_RT      (1u << 4)
#define VB_PAD_LT      (1u << 5)
#define VB_PAD_RUP     (1u << 6)
#define VB_PAD_RRIGHT  (1u << 7)
#define VB_PAD_LRIGHT  (1u << 8)
#define VB_PAD_LLEFT   (1u << 9)
#define VB_PAD_LDOWN   (1u << 10)
#define VB_PAD_LUP     (1u << 11)
#define VB_PAD_START   (1u << 12)
#define VB_PAD_SELECT  (1u << 13)
#define VB_PAD_RLEFT   (1u << 14)
#define VB_PAD_RDOWN   (1u << 15)

void vb_input_init(void);
void vb_input_set_pad(uint16_t pressed);
uint16_t vb_input_get_pad(void);

/* Frame-counted button press for deterministic headless navigation (mirrors
 * the psx/nes `press` debug command): hold `mask` for exactly `frames` game
 * frames, then auto-release. Decremented once per VIP game-frame by
 * vb_input_frame_advance(), so the hold duration is independent of headless
 * wall-clock speed. */
void vb_input_press(uint16_t mask, int frames);
void vb_input_frame_advance(void);
int  vb_input_press_remaining(void);

uint8_t  vb_input_read8 (uint32_t addr);
uint16_t vb_input_read16(uint32_t addr);
uint32_t vb_input_read32(uint32_t addr);
void     vb_input_write8 (uint32_t addr, uint8_t  v);
void     vb_input_write16(uint32_t addr, uint16_t v);
void     vb_input_write32(uint32_t addr, uint32_t v);

#ifdef __cplusplus
}
#endif

#endif /* VB_INPUT_H */
