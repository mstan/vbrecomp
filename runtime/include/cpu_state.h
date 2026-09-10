/* cpu_state.h — V810 CPU state container.
 *
 * Shape modelled on psxrecomp/runtime/include/cpu_state.h. V810
 * substitutes the MIPS R3000 register file and trades GTE for the
 * V810 system-register set + PSW exploded flags.
 */
#ifndef VB_CPU_STATE_H
#define VB_CPU_STATE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* V810 system register indices (LDSR/STSR imm5 field). */
#define VB_SR_EIPC   0
#define VB_SR_EIPSW  1
#define VB_SR_FEPC   2
#define VB_SR_FEPSW  3
#define VB_SR_ECR    4
#define VB_SR_PSW    5
#define VB_SR_PIR    6
#define VB_SR_TKCW   7
#define VB_SR_CHCW   24
#define VB_SR_ADTRE  25

/* PSW bit layout — cross-referenced against Beetle VB
 * (beetle-vb/mednafen/hw_cpu/v810/v810_cpu.h:55-83). */
#define VB_PSW_Z    0x00000001u
#define VB_PSW_S    0x00000002u
#define VB_PSW_OV   0x00000004u
#define VB_PSW_CY   0x00000008u
#define VB_PSW_FPR  0x00000010u
#define VB_PSW_FUD  0x00000020u
#define VB_PSW_FOV  0x00000040u
#define VB_PSW_FZD  0x00000080u
#define VB_PSW_FIV  0x00000100u
#define VB_PSW_FRO  0x00000200u
#define VB_PSW_ID   0x00001000u
#define VB_PSW_AE   0x00002000u
#define VB_PSW_EP   0x00004000u
#define VB_PSW_NP   0x00008000u
#define VB_PSW_IA   0x000F0000u   /* I[19:16] — interrupt-enable level */
#define VB_PSW_IA_SHIFT 16

/* Reset PSW value (Beetle V810::Reset @ v810_cpu.cpp:273):
 * NP=1 — the boot ROM runs with interrupts masked. */
#define VB_PSW_RESET (VB_PSW_NP)

typedef struct CPUState {
    /* General-purpose registers. r0 hardwired to zero by the lifter;
     * the slot is kept for uniform indexing. r29 is sp by convention,
     * r31 is lp (link, written by JAL). */
    uint32_t gpr[32];
    uint32_t pc;

    /* System registers (LDSR/STSR-accessible). Unused indices are
     * defined but writes to NEC-reserved indices route through
     * vb_stub_abort() at the lifter/emitter level. */
    uint32_t sysreg[32];

    /* PSW flags exploded for codegen ergonomics. Keep these in sync
     * with the bit-packed sysreg[VB_SR_PSW] mirror on every flag-update
     * boundary; the recompiled C reads/writes the exploded form. */
    uint8_t psw_z;       /* zero */
    uint8_t psw_s;       /* sign */
    uint8_t psw_ov;      /* signed overflow */
    uint8_t psw_cy;      /* carry / unsigned overflow */
    uint8_t psw_fpr;     /* FP precision (reserved on NVC; modelled for completeness) */
    uint8_t psw_fud;     /* FP underflow */
    uint8_t psw_fov;     /* FP overflow */
    uint8_t psw_fzd;     /* FP zero divide */
    uint8_t psw_fiv;     /* FP invalid */
    uint8_t psw_fro;     /* FP reserved op */
    uint8_t psw_id;      /* interrupt disable */
    uint8_t psw_ep;      /* in-exception */
    uint8_t psw_np;      /* in-NMI */
    uint8_t psw_ae;      /* address-trap enable */
    uint8_t psw_int_level; /* 0..15 */

    /* Wallclock counters. */
    uint64_t cycles;       /* V810 cycle count (rolling) */
    uint32_t frame;        /* 50.27 Hz frame counter */

    /* Set to 1 by the recompiled HALT handler so the runtime's
     * frame scheduler knows to skip cycle-driven dispatch and wait
     * for the next interrupt. Cleared by the interrupt-delivery
     * path (P4+). */
    uint8_t halted;

    /* P3 dispatch budget: vb_dispatch_call decrements this on each
     * inner-switch iteration; when it hits 0 the loop yields to the
     * caller (main loop), allowing TCP polling and external
     * suspend. Disabled when set to UINT64_MAX. The yield path
     * sets `yielded` so the caller can distinguish a budget-exit
     * from a clean JMP r31 return. */
    uint64_t step_budget;
    uint8_t  yielded;

    /* Axis-3 mid-block IRQ-take deadline. The recompiled per-basic-block
     * check yields (exactly like a step_budget exhaustion) once
     * cpu->cycles >= cycle_deadline, so the main loop can tick devices and
     * deliver a now-pending IRQ within one basic block of the true event
     * cycle — instead of running a whole dispatch pass (up to STEP_BUDGET
     * blocks) past it. main.cpp sets this before each pass to
     * cpu->cycles + cycles-to-next-device-event (VIP column / timer divider,
     * the same boundaries the HALT idle loop uses). UINT64_MAX disables it. */
    uint64_t cycle_deadline;
    uint32_t bstr_src_cache;
    uint8_t bstr_src_valid;
    int8_t pipeline_class; /* 0 ALU, -1 long op, 1 LD, 2 ST, 3 IN, 4 OUT */
    uint8_t bus_tail_cycles; /* pipeline charge after the memory access */

    /* Bus function pointers — wired by vb_memory_init(). */
    uint8_t  (*read8) (uint32_t addr);
    uint16_t (*read16)(uint32_t addr);
    uint32_t (*read32)(uint32_t addr);
    void     (*write8) (uint32_t addr, uint8_t  v);
    void     (*write16)(uint32_t addr, uint16_t v);
    void     (*write32)(uint32_t addr, uint32_t v);
} CPUState;

/* Dispatch to recompiled code. Provided by `generated/<name>_dispatch.c`
 * once Phase 3 produces it; the skeleton ships a no_game_linked.c stub
 * that aborts loudly via vb_stub_abort(). */
void vb_dispatch(CPUState* cpu, uint32_t target_pc);
void vb_dispatch_call(CPUState* cpu, uint32_t target_pc, uint32_t lp);

/* CRC32 of the cart this recompiler run was generated against.
 * Returned by the generated `<module>_dispatch.c` so main() can refuse
 * to run against a ROM that doesn't match the recompiled code. A
 * return of 0 means "skip the check" (used by the no-game-linked
 * placeholder build). Defined alongside vb_dispatch in the generated
 * dispatch file. */
uint32_t vb_game_expected_crc32(void);

/* PSW pack/unpack — keep the exploded psw_* fields and
 * sysreg[VB_SR_PSW] in sync.
 *
 * The exploded form is authoritative during normal execution (every
 * Format-I ALU updates it). The packed form (sysreg[5]) is what LDSR
 * writes / STSR reads / vb_trap saves into EIPSW / vb_reti restores
 * from EIPSW. Without the helpers, LDSR-to-PSW would update only the
 * packed slot and leave the exploded fields stale (and the recompiled
 * code reads the exploded form on every flag-consuming instruction).
 *
 * Defined inline so the call is free in -O2 codegen. */
static inline uint32_t vb_psw_pack(const CPUState* cpu) {
    return  ((uint32_t)cpu->psw_z   ? VB_PSW_Z   : 0u)
         |  ((uint32_t)cpu->psw_s   ? VB_PSW_S   : 0u)
         |  ((uint32_t)cpu->psw_ov  ? VB_PSW_OV  : 0u)
         |  ((uint32_t)cpu->psw_cy  ? VB_PSW_CY  : 0u)
         |  ((uint32_t)cpu->psw_fpr ? VB_PSW_FPR : 0u)
         |  ((uint32_t)cpu->psw_fud ? VB_PSW_FUD : 0u)
         |  ((uint32_t)cpu->psw_fov ? VB_PSW_FOV : 0u)
         |  ((uint32_t)cpu->psw_fzd ? VB_PSW_FZD : 0u)
         |  ((uint32_t)cpu->psw_fiv ? VB_PSW_FIV : 0u)
         |  ((uint32_t)cpu->psw_fro ? VB_PSW_FRO : 0u)
         |  ((uint32_t)cpu->psw_id  ? VB_PSW_ID  : 0u)
         |  ((uint32_t)cpu->psw_ae  ? VB_PSW_AE  : 0u)
         |  ((uint32_t)cpu->psw_ep  ? VB_PSW_EP  : 0u)
         |  ((uint32_t)cpu->psw_np  ? VB_PSW_NP  : 0u)
         |  (((uint32_t)cpu->psw_int_level & 0xFu) << VB_PSW_IA_SHIFT);
}

static inline void vb_psw_unpack(CPUState* cpu, uint32_t v) {
    cpu->psw_z         = (v & VB_PSW_Z)   ? 1u : 0u;
    cpu->psw_s         = (v & VB_PSW_S)   ? 1u : 0u;
    cpu->psw_ov        = (v & VB_PSW_OV)  ? 1u : 0u;
    cpu->psw_cy        = (v & VB_PSW_CY)  ? 1u : 0u;
    cpu->psw_fpr       = (v & VB_PSW_FPR) ? 1u : 0u;
    cpu->psw_fud       = (v & VB_PSW_FUD) ? 1u : 0u;
    cpu->psw_fov       = (v & VB_PSW_FOV) ? 1u : 0u;
    cpu->psw_fzd       = (v & VB_PSW_FZD) ? 1u : 0u;
    cpu->psw_fiv       = (v & VB_PSW_FIV) ? 1u : 0u;
    cpu->psw_fro       = (v & VB_PSW_FRO) ? 1u : 0u;
    cpu->psw_id        = (v & VB_PSW_ID)  ? 1u : 0u;
    cpu->psw_ae        = (v & VB_PSW_AE)  ? 1u : 0u;
    cpu->psw_ep        = (v & VB_PSW_EP)  ? 1u : 0u;
    cpu->psw_np        = (v & VB_PSW_NP)  ? 1u : 0u;
    cpu->psw_int_level = (uint8_t)((v & VB_PSW_IA) >> VB_PSW_IA_SHIFT);
    cpu->sysreg[VB_SR_PSW] = v & (VB_PSW_Z | VB_PSW_S | VB_PSW_OV | VB_PSW_CY
                               |  VB_PSW_FPR | VB_PSW_FUD | VB_PSW_FOV
                               |  VB_PSW_FZD | VB_PSW_FIV | VB_PSW_FRO
                               |  VB_PSW_ID  | VB_PSW_AE  | VB_PSW_EP
                               |  VB_PSW_NP  | VB_PSW_IA);
}

/* Reset cpu state to V810 power-on values
 * (Beetle V810::Reset @ v810_cpu.cpp:261-289). Bus pointers are
 * preserved — they're wired by vb_memory_init() and live longer than
 * a CPU reset. */
void vb_cpu_reset(CPUState* cpu);

/* ---- Per-instruction CPU-hook (Axis 1/2/3/6 divergence harness) -------
 * Built with -DVB_CPUHOOK_ENABLE (CMake: -DVBRECOMP_CPUHOOK=ON), the
 * emitter's per-instruction VB_CPUHOOK(cpu) records {pc, packed PSW,
 * regs-FNV, cycle} into an always-on ring (cpuhook.c) for diffing against
 * the oracle's RB_CPUHOOK stream (first divergence + cycle Delta). The
 * record is taken BEFORE the instruction's cycle charge and body, so it
 * captures pre-instruction state/cycle — matching the oracle, which hooks
 * before execution. Default build: the macro is a no-op (zero overhead,
 * behavior byte-identical), mirroring the opt-in overrides convention. */
#ifdef VB_CPUHOOK_ENABLE
void vb_cpuhook_record(const CPUState* cpu);
#define VB_CPUHOOK(cpu) vb_cpuhook_record(cpu)
#else
#define VB_CPUHOOK(cpu) ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif /* VB_CPU_STATE_H */
