/* Virtual Boy controller: hardware bit masks, byte-wide data ports, and
 * the 16 x 640-cycle hardware serial transfer with busy/abort/IRQ behavior.
 * SDR exposes the serially latched bits, including the previous transfer's
 * bits until the new transfer replaces them. Reference: beetle-vb input.c. */
#include "input.h"
#include "interrupts.h"

static int32_t s_read_counter;
static unsigned s_read_bit;
static uint16_t s_latched,s_serial;
static uint8_t s_scr;
void vb_input_tick(uint32_t cycles) {
    if(s_read_counter<=0) return;
    s_read_counter-=(int32_t)cycles;
    while(s_read_counter<=0) {
        uint16_t bit=(uint16_t)(1u<<s_read_bit);
        s_serial=(uint16_t)((s_serial&~bit)|(s_latched&bit));
        if(++s_read_bit==16) {
            if(!(s_scr&0x80)) vb_irq_assert(VBIRQ_SOURCE_INPUT,1);
            break;
        }
        s_read_counter+=640;
    }
}
int32_t vb_input_cycles_to_next_event(void) {
    return s_read_counter>0 ? s_read_counter:0x3fffffff;
}

#include "stub_abort.h"

static uint16_t s_pad;

/* Frame-counted press state (debug navigation; see vb_input_press). */
static uint16_t s_press_mask;
static int      s_press_frames;

void vb_input_init(void) {
    /* Bit 1 ("device-connected" sentinel) must always be 1 so the
     * cart's controller-presence check passes. Beetle composes
     * PadData with `| 0x2` for the same reason. */
    s_pad = VB_PAD_PRESENT;
    s_scr = 0;
    s_read_counter=0;s_read_bit=0;s_latched=s_serial=0;
    vb_irq_assert(VBIRQ_SOURCE_INPUT,0);
    s_press_mask = 0;
    s_press_frames = 0;
}

void vb_input_press(uint16_t mask, int frames) {
    if (frames < 1) frames = 1;
    s_press_mask = mask;
    s_press_frames = frames;
    vb_input_set_pad(mask);
}

void vb_input_frame_advance(void) {
    if (s_press_frames <= 0) return;
    if (--s_press_frames == 0) {
        s_press_mask = 0;
        vb_input_set_pad(0);
    } else {
        vb_input_set_pad(s_press_mask);  /* keep holding */
    }
}

int vb_input_press_remaining(void) { return s_press_frames; }

void vb_input_set_pad(uint16_t pressed) {
    /* Caller passes a "buttons currently pressed" mask using the
     * VB_PAD_* hardware-bit macros. Always re-force the PRESENT
     * sentinel so the cart sees a live controller. */
    s_pad = (uint16_t)((pressed & ~(uint16_t)VB_PAD_BAT_LOW) | VB_PAD_PRESENT);
}
uint16_t vb_input_get_pad(void) { return s_pad; }

static uint8_t input_read_low_byte(uint32_t lo) {
    switch (lo) {
        case 0x10: return (uint8_t)(s_serial & 0xFFu);
        case 0x11: return 0;                          /* misaligned → 0 */
        case 0x14: return (uint8_t)((s_serial >> 8) & 0xFFu);
        case 0x15: return 0;
        case 0x28:
            /* SCR reports the in-progress serial transfer. */
            return (uint8_t)(s_scr | 0x4cu | (s_read_counter>0 ? 2u:0u));
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
        case 0x10: return (uint8_t)s_serial;
        case 0x14: return (uint16_t)(s_serial >> 8);
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
        if((v&4) && !(s_scr&1) && s_read_counter<=0) {
            s_latched=s_pad;s_read_bit=0;s_read_counter=640;
        }
        if(v&1) { s_read_counter=0; }
        if(v&0x80) vb_irq_assert(VBIRQ_SOURCE_INPUT,0);
        /* Stored control bits exclude command and busy bits. */
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

/* Canonical, read-only device state. Ordered schema: tools/device_schema.json. */
#ifdef VBRECOMP_DEBUG_TOOLS
unsigned vb_input_snapshot(uint32_t* out) {
    const uint32_t words[] = {
        (uint32_t)(s_pad), /* pad */
        (uint32_t)(s_latched), /* latched */
        (uint32_t)(s_scr), /* control */
        (uint32_t)(s_serial), /* serial */
        (uint32_t)(s_read_bit), /* bit */
        (uint32_t)(s_read_counter), /* counter */
        (uint32_t)((vb_irq_pending() & 1)), /* irq */
    };
    unsigned n=sizeof(words)/sizeof(words[0]);
    for(unsigned i=0;i<n;++i) out[i]=words[i];
    return n;
}
#endif
