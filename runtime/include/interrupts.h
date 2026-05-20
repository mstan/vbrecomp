/* interrupts.h — VBIRQ controller + V810 exception entry.
 *
 * Source ID layout matches Beetle VB
 * (beetle-vb/mednafen/vb/vb.h:28-32). Higher numeric ID = higher
 * priority on the V810 (level = source ID, vector = 0xFFFFFE00 +
 * (level << 4)).
 *
 * V810 exception/interrupt entry shape cross-checked against
 *   beetle-vb/mednafen/hw_cpu/v810/v810_oploop.inc:977-1009  (interrupt)
 *   beetle-vb/mednafen/hw_cpu/v810/v810_cpu.cpp:1111-1151    (Exception)
 *   beetle-vb/mednafen/hw_cpu/v810/v810_oploop.inc:793-809   (RETI)
 *   beetle-vb/mednafen/hw_cpu/v810/v810_oploop.inc:924-931   (TRAP)
 */
#ifndef VB_INTERRUPTS_H
#define VB_INTERRUPTS_H

#include <stdint.h>
#include "cpu_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Source IDs == V810 interrupt levels. */
#define VBIRQ_SOURCE_INPUT      0u
#define VBIRQ_SOURCE_TIMER      1u
#define VBIRQ_SOURCE_EXPANSION  2u
#define VBIRQ_SOURCE_COMM       3u
#define VBIRQ_SOURCE_VIP        4u
#define VBIRQ_SOURCE_COUNT      5u

/* Exception cause codes (Beetle v810_cpu.h:30-37). */
#define VB_ECODE_TRAP_BASE        0xFFA0u
#define VB_ECODE_INVALID_OP       0xFF90u
#define VB_ECODE_ZERO_DIV         0xFF80u
#define VB_ECODE_FRO              0xFF60u
#define VB_ECODE_FIV              0xFF70u
#define VB_ECODE_FZD              0xFF68u
#define VB_ECODE_FOV              0xFF64u
#define VB_ECODE_FUD              0xFF62u
#define VB_ECODE_FPR              0xFF61u

#define VB_TRAP_HANDLER_BASE      0xFFFFFFA0u
#define VB_INVALID_OP_HANDLER     0xFFFFFF90u
#define VB_ZERO_DIV_HANDLER       0xFFFFFF80u
#define VB_FPU_HANDLER            0xFFFFFF60u
#define VB_DOUBLE_FAULT_HANDLER   0xFFFFFFD0u

void vb_irq_init(void);

/* Assert (`asserted!=0`) or deassert a source line. Sources are
 * level-triggered: the IRQ controller re-asserts immediately if the
 * underlying state still says active. Beetle equivalent:
 * VBIRQ_Assert(source, bool) in vb.cpp. */
void vb_irq_assert(uint32_t source, int asserted);

/* Convenience for one-shot triggers (mostly the debug harness:
 * irq_force command). Equivalent to vb_irq_assert(source, 1) followed
 * by an immediate ack window managed by the caller. */
void vb_irq_raise(uint32_t source);
void vb_irq_clear(uint32_t source);

/* Pending mask + level of the highest-priority pending source.
 * Returns -1 in `out_level` if nothing pending. */
uint32_t vb_irq_pending(void);
uint32_t vb_irq_in_service(void);
int      vb_irq_highest_pending_level(void);

/* Returns 1 if an IRQ was delivered (cpu->pc was retargeted to a
 * vector, sysreg's saved, PSW updated). 0 otherwise.
 *
 * Acceptance rule (Beetle V810::RecalcIPendingCache @ v810_cpu.cpp:89):
 *   - PSW & (NP | EP | ID) == 0
 *   - pending level >= PSW.IA */
int vb_irq_check_and_deliver(CPUState* cpu);

/* V810 exception entry — used by the emitter for TRAP and by the
 * runtime for synthesised exceptions (invalid op, divide by zero,
 * FPU). Mutates cpu->pc, cpu->sysreg, cpu->psw_*.
 *
 *   handler — vector address (0xFFFFFF60..0xFFFFFFBF for sw exceptions,
 *             0xFFFFFE00..0xFFFFFE40 for interrupts; the latter goes
 *             through vb_irq_check_and_deliver, NOT this entry).
 *   ecode   — value latched into ECR (low 16 for regular, high 16 for
 *             double-fault).
 */
void vb_exception(CPUState* cpu, uint32_t handler, uint16_t ecode);

/* TRAP imm5 helper — Beetle v810_oploop.inc:929. */
void vb_trap(CPUState* cpu, uint32_t imm5);

/* RETI — Beetle v810_oploop.inc:793. PC restore + PSW restore from
 * FE* (if PSW.NP) or EI* (otherwise). cpu->pc is set; the caller
 * (recompiled emitter) must `return` immediately so the dispatch
 * loop re-routes to the restored PC. */
void vb_reti(CPUState* cpu);

#ifdef __cplusplus
}
#endif

#endif /* VB_INTERRUPTS_H */
