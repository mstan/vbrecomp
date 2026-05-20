/* interrupts.c — VBIRQ controller + V810 exception entry.
 *
 * Cross-checked against:
 *   beetle-vb/mednafen/hw_cpu/v810/v810_cpu.cpp:1111-1151   Exception()
 *   beetle-vb/mednafen/hw_cpu/v810/v810_oploop.inc:977-1009  op_INT_HANDLER
 *   beetle-vb/mednafen/hw_cpu/v810/v810_oploop.inc:793-809   op_RETI
 *   beetle-vb/mednafen/hw_cpu/v810/v810_oploop.inc:924-931   op_TRAP
 *   beetle-vb/mednafen/hw_cpu/v810/v810_cpu.cpp:89-113       RecalcIPendingCache
 */
#include "interrupts.h"

#include "stub_abort.h"

#include <stdint.h>

/* Pending = level-triggered source-line state. A source-active edge
 * sets the bit; clearing the source line (e.g. timer ZSTATCLR write,
 * VIP INTCLR) clears it. The pending bit IS the source line — there
 * is no separate latch on top. Beetle keeps the same invariant. */
static uint32_t s_pending = 0;

/* In-service mask is informational only — the V810 doesn't have a
 * hardware in-service register; software-visible state lives in PSW
 * and EIPC/EIPSW. We track it for the debug server's irq_state
 * command so callers can tell which level the CPU last entered. */
static uint32_t s_in_service = 0;

void vb_irq_init(void) {
    s_pending    = 0;
    s_in_service = 0;
}

void vb_irq_assert(uint32_t source, int asserted) {
    if (source >= VBIRQ_SOURCE_COUNT) {
        vb_stub_abort("vb_irq_assert: source out of range",
                      0, source);
    }
    if (asserted) s_pending |=  (1u << source);
    else          s_pending &= ~(1u << source);
}

void vb_irq_raise(uint32_t source) { vb_irq_assert(source, 1); }
void vb_irq_clear(uint32_t source) { vb_irq_assert(source, 0); }

uint32_t vb_irq_pending(void)    { return s_pending; }
uint32_t vb_irq_in_service(void) { return s_in_service; }

int vb_irq_highest_pending_level(void) {
    /* Highest source bit set == highest level. VIP=4 outranks
     * COMM=3 outranks EXPANSION=2 outranks TIMER=1 outranks
     * INPUT=0. Matches Beetle's `ilevel` tracking. */
    for (int level = (int)VBIRQ_SOURCE_COUNT - 1; level >= 0; --level) {
        if (s_pending & (1u << level))
            return level;
    }
    return -1;
}

int vb_irq_check_and_deliver(CPUState* cpu) {
    int level = vb_irq_highest_pending_level();
    if (level < 0) return 0;

    /* Acceptance gate. Match Beetle V810::RecalcIPendingCache. */
    if (cpu->psw_np || cpu->psw_ep || cpu->psw_id) return 0;
    if ((int)cpu->psw_int_level > level) return 0;

    /* Beetle op_INT_HANDLER: save PC + PSW, retarget to vector,
     * set EP|ID, clear AE, raise interrupt-enable level by 1. */
    cpu->sysreg[VB_SR_EIPC]  = cpu->pc;
    cpu->sysreg[VB_SR_EIPSW] = vb_psw_pack(cpu);

    uint32_t vector = 0xFFFFFE00u | ((uint32_t)level << 4);
    cpu->pc                  = vector;
    cpu->sysreg[VB_SR_ECR]   = (uint32_t)(0xFE00u | ((uint32_t)level << 4));

    cpu->psw_ep        = 1;
    cpu->psw_id        = 1;
    cpu->psw_ae        = 0;
    int next_level = level + 1;
    if (next_level > 0x0F) next_level = 0x0F;
    cpu->psw_int_level = (uint8_t)next_level;
    cpu->sysreg[VB_SR_PSW] = vb_psw_pack(cpu);

    cpu->halted     = 0;
    s_in_service   |= (1u << level);
    return 1;
}

void vb_exception(CPUState* cpu, uint32_t handler, uint16_t ecode) {
    /* Beetle V810::Exception (v810_cpu.cpp:1111). */
    if (cpu->psw_np) {
        /* Fatal exception — Beetle halts; we abort, since on a
         * static recompiler a fatal exception cannot be recovered
         * from without a guest-installed handler. */
        vb_stub_abort("V810 fatal exception (PSW.NP already set when "
                      "vb_exception fired); ECR not modelled in halt",
                      cpu->pc, ecode);
    }

    if (cpu->psw_ep) {
        /* Double exception: save FE registers, jump to double-fault
         * vector, set NP|ID, clear AE. */
        cpu->sysreg[VB_SR_FEPC]  = cpu->pc;
        cpu->sysreg[VB_SR_FEPSW] = vb_psw_pack(cpu);
        cpu->sysreg[VB_SR_ECR]   = (cpu->sysreg[VB_SR_ECR] & 0xFFFFu)
                                 | ((uint32_t)ecode << 16);
        cpu->psw_np = 1;
        cpu->psw_id = 1;
        cpu->psw_ae = 0;
        cpu->sysreg[VB_SR_PSW] = vb_psw_pack(cpu);
        cpu->pc = VB_DOUBLE_FAULT_HANDLER;
        return;
    }

    /* Regular exception. */
    cpu->sysreg[VB_SR_EIPC]  = cpu->pc;
    cpu->sysreg[VB_SR_EIPSW] = vb_psw_pack(cpu);
    cpu->sysreg[VB_SR_ECR]   = (cpu->sysreg[VB_SR_ECR] & 0xFFFF0000u)
                             | (uint32_t)ecode;
    cpu->psw_ep = 1;
    cpu->psw_id = 1;
    cpu->psw_ae = 0;
    cpu->sysreg[VB_SR_PSW] = vb_psw_pack(cpu);
    cpu->pc = handler;
}

void vb_trap(CPUState* cpu, uint32_t imm5) {
    /* Beetle op_TRAP: handler = TRAP_HANDLER_BASE + (imm5 & 0x10),
     * ecode = ECODE_TRAP_BASE + (imm5 & 0x1F). */
    uint32_t handler = VB_TRAP_HANDLER_BASE + (imm5 & 0x10u);
    uint16_t ecode   = (uint16_t)(VB_ECODE_TRAP_BASE + (imm5 & 0x1Fu));
    vb_exception(cpu, handler, ecode);
}

void vb_reti(CPUState* cpu) {
    /* Beetle op_RETI. */
    if (cpu->psw_np) {
        cpu->pc = cpu->sysreg[VB_SR_FEPC] & 0xFFFFFFFEu;
        vb_psw_unpack(cpu, cpu->sysreg[VB_SR_FEPSW]);
    } else {
        cpu->pc = cpu->sysreg[VB_SR_EIPC] & 0xFFFFFFFEu;
        vb_psw_unpack(cpu, cpu->sysreg[VB_SR_EIPSW]);
    }
    /* No in-service bookkeeping — the V810 has no hardware
     * in-service register. The IRQ controller's s_in_service bit
     * for this level is cleared by software's acknowledge write
     * (INTCLR / TCR.ZSTATCLR / INPUT clear) to the source. */
}

void vb_cpu_reset(CPUState* cpu) {
    /* Preserve bus-function pointers (they live longer than a CPU
     * reset). Wipe everything else and apply Beetle V810::Reset
     * defaults. */
    uint8_t  (*r8)(uint32_t)            = cpu->read8;
    uint16_t (*r16)(uint32_t)           = cpu->read16;
    uint32_t (*r32)(uint32_t)           = cpu->read32;
    void     (*w8)(uint32_t, uint8_t)   = cpu->write8;
    void     (*w16)(uint32_t, uint16_t) = cpu->write16;
    void     (*w32)(uint32_t, uint32_t) = cpu->write32;

    for (int i = 0; i < 32; ++i) cpu->gpr[i] = 0;
    for (int i = 0; i < 32; ++i) cpu->sysreg[i] = 0;

    cpu->pc                = 0xFFFFFFF0u;
    cpu->sysreg[VB_SR_ECR] = 0x0000FFF0u;
    cpu->sysreg[VB_SR_PIR] = 0x00005346u;   /* VB-mode PIR (Beetle line 276) */
    cpu->sysreg[VB_SR_TKCW] = 0x000000E0u;

    vb_psw_unpack(cpu, VB_PSW_RESET);

    cpu->cycles      = 0;
    cpu->frame       = 0;
    cpu->halted      = 0;
    cpu->step_budget = 0;
    cpu->yielded     = 0;

    cpu->read8  = r8;
    cpu->read16 = r16;
    cpu->read32 = r32;
    cpu->write8 = w8;
    cpu->write16 = w16;
    cpu->write32 = w32;
}
