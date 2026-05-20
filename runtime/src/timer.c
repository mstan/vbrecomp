/* timer.c — 20-bit programmable down-counter.
 *
 * Behavioural oracle: Beetle VB
 * (beetle-vb/mednafen/vb/timer.c TIMER_Update / TIMER_Read /
 * TIMER_Write / TIMER_Power).
 *
 * Mechanics:
 *   - Two clocks: 100µs (2000 V810 cycles, TCLKSEL=0) or
 *     20µs (400 cycles, TCLKSEL=1).
 *   - Each tick, if counter is 0 or a reload is pending the counter
 *     reloads from TimerReloadValue, then decrements. Underflow
 *     latches TimerStatus + TimerStatusShadow.
 *   - VBIRQ_SOURCE_TIMER is asserted while
 *     `TimerStatusShadow && (TCR & TIMZINT)`.
 *   - Software clears ZSTAT by writing 1 to TC_ZSTATCLR. Quirk:
 *     while TENABLE && TimerCounter == 0, the clear bit is masked
 *     by hardware (Beetle: "Faulty Z-Stat-Clr").
 *   - Enabling TENABLE seeds TimerDivider to 500 (fast) or 2000
 *     (slow) — a real-hardware quirk where fast mode loses 100
 *     cycles on the first tick after enable.
 *
 * Cycle ticking: vb_timer_tick(N) is called by main.cpp once per
 * dispatch-yield boundary with the V810-cycle delta consumed by the
 * recompiled cart. The function loops the divider until it has
 * absorbed all N cycles, taking the underflow path per tick.
 */
#include "timer.h"

#include "interrupts.h"
#include "stub_abort.h"

static uint8_t  s_tcr;
static uint16_t s_reload;
static uint16_t s_counter;
static int32_t  s_divider;
static uint8_t  s_zstat;          /* TimerStatus */
static uint8_t  s_zstat_shadow;   /* TimerStatusShadow */
static uint8_t  s_reload_pending;

static void timer_reassert_irq(void) {
    int asserted = s_zstat_shadow && (s_tcr & VB_TCR_TIMZINT);
    vb_irq_assert(VBIRQ_SOURCE_TIMER, asserted);
}

static int32_t timer_divider_period(void) {
    return (s_tcr & VB_TCR_TCLKSEL) ? 400 : 2000;
}

void vb_timer_init(void) {
    /* Beetle TIMER_Power. */
    s_tcr          = 0;
    s_reload       = 0xFFFFu;
    s_counter      = 0xFFFFu;
    s_divider      = 2000;
    s_zstat        = 0;
    s_zstat_shadow = 0;
    s_reload_pending = 0;
    vb_irq_assert(VBIRQ_SOURCE_TIMER, 0);
}

void vb_timer_tick(uint32_t cycles) {
    if (!(s_tcr & VB_TCR_TENABLE) || cycles == 0)
        return;
    s_divider -= (int32_t)cycles;
    while (s_divider <= 0) {
        if (s_counter == 0 || s_reload_pending) {
            s_counter = s_reload;
            s_reload_pending = 0;
        }
        if (s_counter)
            s_counter--;
        if (s_counter == 0 || s_zstat) {
            s_zstat = 1;
            s_zstat_shadow = 1;
        }
        timer_reassert_irq();
        s_divider += timer_divider_period();
    }
}

uint8_t vb_timer_read8(uint32_t addr) {
    /* Misaligned reads return 0 (Beetle TIMER_Read default branch). */
    switch (addr & 0xFFu) {
        case 0x18: return (uint8_t)(s_counter & 0xFF);
        case 0x1C: return (uint8_t)((s_counter >> 8) & 0xFF);
        case 0x20:
            return (uint8_t)(s_tcr
                             | 0xE0u
                             | VB_TCR_ZSTATCLR
                             | (s_zstat ? VB_TCR_ZSTAT : 0u));
        default:
            return 0;
    }
}

uint16_t vb_timer_read16(uint32_t addr) {
    /* Reads are byte-wide in hardware. A halfword read at 0x18
     * fetches counter-low (0x18) and a follow-up 0x19 which decodes
     * as 0 per Beetle's default branch. */
    return (uint16_t)vb_timer_read8(addr);
}

uint32_t vb_timer_read32(uint32_t addr) {
    return (uint32_t)vb_timer_read8(addr);
}

void vb_timer_write8(uint32_t addr, uint8_t v) {
    /* Misaligned writes are dropped (Beetle TIMER_Write line 94). */
    if (addr & 0x3u)
        return;
    switch (addr & 0xFFu) {
        case 0x18:
            s_reload = (uint16_t)((s_reload & 0xFF00u) | v);
            s_reload_pending = 1;
            break;
        case 0x1C:
            s_reload = (uint16_t)((s_reload & 0x00FFu) | ((uint16_t)v << 8));
            s_reload_pending = 1;
            break;
        case 0x20: {
            uint8_t new_tcr = (uint8_t)(v & (VB_TCR_TCLKSEL
                                           | VB_TCR_TIMZINT
                                           | VB_TCR_TENABLE));
            if (v & VB_TCR_ZSTATCLR) {
                /* Beetle's "Faulty Z-Stat-Clr": while running with
                 * counter underflowed, the clear is suppressed. */
                if ((s_tcr & VB_TCR_TENABLE) && s_counter == 0) {
                    /* Suppressed — keep s_zstat as-is. */
                } else {
                    s_zstat = 0;
                }
                s_zstat_shadow = 0;
            }
            if ((new_tcr & VB_TCR_TENABLE) && !(s_tcr & VB_TCR_TENABLE)) {
                /* First-tick quirk: fast mode seeds at 500 cycles. */
                s_divider = (new_tcr & VB_TCR_TCLKSEL) ? 500 : 2000;
            }
            s_tcr = new_tcr;
            if (!(s_tcr & VB_TCR_TIMZINT)) {
                s_zstat = 0;
                s_zstat_shadow = 0;
            }
            timer_reassert_irq();
            break;
        }
        default:
            /* Other offsets in the timer register window are
             * undefined; treat as harmless writes for now (Beetle
             * does the same: the switch falls through). If a real
             * cart writes here we'll see it via the misc-page log. */
            break;
    }
}

void vb_timer_write16(uint32_t addr, uint16_t v) {
    vb_timer_write8(addr, (uint8_t)(v & 0xFFu));
}

void vb_timer_write32(uint32_t addr, uint32_t v) {
    vb_timer_write8(addr, (uint8_t)(v & 0xFFu));
}

uint16_t vb_timer_counter(void) { return s_counter; }
uint16_t vb_timer_reload(void)  { return s_reload; }
uint8_t  vb_timer_control(void) { return s_tcr; }
int32_t  vb_timer_divider(void) { return s_divider; }
