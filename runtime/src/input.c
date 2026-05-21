/* input.c — Virtual Boy pad register.
 *
 * Beetle's VBINPUT (beetle-vb/mednafen/vb/input.c) exposes three
 * misc-page registers in the input set:
 *   0x10 — SDR_LO  (serial data register low; with InstantReadHack
 *                   returns PadData low byte)
 *   0x14 — SDR_HI  (serial data register high; with InstantReadHack
 *                   returns PadData high byte)
 *   0x28 — SCR     (serial control register)
 *
 * Beetle defaults `InstantReadHack=true` (input.c:52) so 0x10/0x14
 * return the current pad state directly. Implementing the serial-
 * shift mechanic (640 cycles per bit, INPUT IRQ on completion) is a
 * P5 concern when actual pad input matters — for P4-B the cart's
 * register pokes don't depend on it. The 16-bit pad value is
 * exposed by `vb_input_get_pad()` and bus reads return its bytes.
 *
 * SCR writes accept the bits Beetle stores (0x91) without modelling
 * the IRQ side-effects yet — same P5 deferral. We don't fabricate a
 * status: SCR_HW_SI / SCR_SI_STAT read-back is the actual state of
 * the shift register, which (without the shift mechanic) is "idle"
 * (Beetle reads 0x40 | 0x08 | SCR_HW_SI = 0x4C).
 */
#include "input.h"

#include "stub_abort.h"

static uint16_t s_pad;
static uint8_t  s_scr;

void vb_input_init(void) {
    /* Bit 1 ("device-connected" sentinel) must always be 1 so the
     * cart's controller-presence check passes. Beetle composes
     * PadData with `| 0x2` for the same reason. */
    s_pad = VB_PAD_PRESENT;
    s_scr = 0;
}

void vb_input_set_pad(uint16_t pressed) {
    /* Caller passes a "buttons currently pressed" mask using the
     * VB_PAD_* hardware-bit macros. Always re-force the PRESENT
     * sentinel so the cart sees a live controller. */
    s_pad = (uint16_t)((pressed & ~(uint16_t)VB_PAD_BAT_LOW) | VB_PAD_PRESENT);
}
uint16_t vb_input_get_pad(void) { return s_pad; }

static uint8_t input_read_low_byte(uint32_t lo) {
    switch (lo) {
        case 0x10: return (uint8_t)(s_pad & 0xFFu);
        case 0x11: return 0;                          /* misaligned → 0 */
        case 0x14: return (uint8_t)((s_pad >> 8) & 0xFFu);
        case 0x15: return 0;
        case 0x28:
            /* Beetle: SCR | (0x40 | 0x08 | SCR_HW_SI). SI_STAT bit set
             * during a shift; we don't shift yet so leave it 0. */
            return (uint8_t)(s_scr | 0x40u | 0x08u | 0x04u);
        case 0x29: return 0;
        default:   return 0;
    }
}

uint8_t vb_input_read8(uint32_t addr) {
    return input_read_low_byte(addr & 0xFFu);
}

uint16_t vb_input_read16(uint32_t addr) {
    uint32_t lo = addr & 0xFFu;
    switch (lo) {
        case 0x10: return s_pad;
        case 0x14: return (uint16_t)(s_pad >> 8);     /* matches Beetle's byte-level read */
        case 0x28: return (uint16_t)input_read_low_byte(0x28);
        default:   return 0;
    }
}

uint32_t vb_input_read32(uint32_t addr) {
    return (uint32_t)vb_input_read16(addr);
}

void vb_input_write8(uint32_t addr, uint8_t v) {
    uint32_t lo = addr & 0xFFu;
    if (lo == 0x28) {
        /* SCR write: Beetle stores `V & (0x80 | 0x20 | 0x10 | 1)`. We
         * accept and store; the IRQ-bearing side-effects (HW_SI starts
         * a 640-cycle shift, K_INT_INH clears INPUT IRQ) are P5. */
        s_scr = (uint8_t)(v & (0x80u | 0x20u | 0x10u | 1u));
        return;
    }
    /* Beetle drops writes to 0x10 / 0x14 (read-only SDR). */
}

void vb_input_write16(uint32_t addr, uint16_t v) {
    vb_input_write8(addr, (uint8_t)(v & 0xFFu));
}

void vb_input_write32(uint32_t addr, uint32_t v) {
    vb_input_write8(addr, (uint8_t)(v & 0xFFu));
}
