/* timer.h — 20-bit programmable timer surface.
 *
 * Register-page semantics cross-checked against Beetle VB
 * (beetle-vb/mednafen/vb/timer.c). The misc-page bus uses a 4-byte
 * stride: only addresses with (A & 3) == 0 carry the register, and
 * writes with (A & 3) != 0 are silently dropped (Beetle TIMER_Write
 * line 94). Misaligned reads return 0 (the default branch of the
 * Beetle case-switch in TIMER_Read).
 *
 * Note: the original Phase-1 stub used THR=0x0200001A on a 2-byte
 * stride. That was wrong — Beetle decodes the high byte at 0x1C and
 * rejects 0x1A as out-of-band. Corrected here before Phase 4-A uses
 * the timer for real IRQ generation. */
#ifndef VB_TIMER_H
#define VB_TIMER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VB_TIMER_TLR   0x02000018u   /* counter low (R) / reload low (W) */
#define VB_TIMER_THR   0x0200001Cu   /* counter high (R) / reload high (W) */
#define VB_TIMER_TCR   0x02000020u   /* control */

/* TCR bit definitions (Beetle timer.c:23-27). */
#define VB_TCR_TENABLE   0x01u
#define VB_TCR_ZSTAT     0x02u   /* underflow latch; read-only */
#define VB_TCR_ZSTATCLR  0x04u   /* write 1 to clear ZSTAT */
#define VB_TCR_TIMZINT   0x08u   /* IRQ enable */
#define VB_TCR_TCLKSEL   0x10u   /* 0 = 100µs (2000 cyc), 1 = 20µs (400 cyc) */

void vb_timer_init(void);

/* Advance the timer by `cycles` V810 cycles. Called by main.cpp at
 * each dispatch-yield boundary with the cycle delta accumulated by
 * the recompiled cart since the last call. May assert
 * VBIRQ_SOURCE_TIMER via the IRQ controller on underflow. */
void vb_timer_tick(uint32_t cycles);

uint8_t  vb_timer_read8 (uint32_t addr);
uint16_t vb_timer_read16(uint32_t addr);
uint32_t vb_timer_read32(uint32_t addr);
void     vb_timer_write8 (uint32_t addr, uint8_t  v);
void     vb_timer_write16(uint32_t addr, uint16_t v);
void     vb_timer_write32(uint32_t addr, uint32_t v);

/* Debug introspection (TCP). */
uint16_t vb_timer_counter(void);
uint16_t vb_timer_reload(void);
uint8_t  vb_timer_control(void);
int32_t  vb_timer_divider(void);

#ifdef __cplusplus
}
#endif

#endif /* VB_TIMER_H */
