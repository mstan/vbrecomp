/* no_game_linked.c — Phase 1 dispatch placeholders.
 *
 * When no `generated/<name>_dispatch.c` has been produced by the
 * recompiler yet (the Phase 1 case), the runtime still needs vb_dispatch
 * + vb_dispatch_call symbols to link. Both unconditionally abort via
 * vb_stub_abort(): the runtime is fine to start its TCP server and idle,
 * but any attempt to actually execute V810 code is a hard error.
 *
 * Once Phase 3 ships a real dispatch table, the runtime.cmake build
 * drops this file from the source list. There is no soft handover —
 * either you have generated dispatch, or you don't.
 */
#include "cpu_state.h"
#include "stub_abort.h"

void vb_dispatch(CPUState* cpu, uint32_t target_pc) {
    (void)cpu;
    vb_stub_abort(
        "vb_dispatch called but no generated game C is linked into this "
        "build. This is the Phase 1 skeleton — only TCP harness validation "
        "is supported. Generate a game (Phase 3) and rebuild with the "
        "generated dispatch source list.",
        target_pc, 0xFFFFFFFFu);
}

void vb_dispatch_call(CPUState* cpu, uint32_t target_pc, uint32_t lp) {
    (void)cpu;
    (void)lp;
    vb_stub_abort(
        "vb_dispatch_call called but no generated game C is linked",
        target_pc, 0xFFFFFFFFu);
}

/* No game linked → no expected CRC → main skips the check. */
uint32_t vb_game_expected_crc32(void) { return 0; }
