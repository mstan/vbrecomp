/* watchdog.h — main-loop hang detector (always-on).
 *
 * The vb-runtime main loop is single-threaded: CPU dispatch, device ticks,
 * SDL present and the TCP debug-server poll all run on one thread. If any of
 * those stalls (an internal infinite loop, a blocking SDL/audio call, a spin
 * that never yields), the window stops pumping Win32 messages and shows
 * "Not Responding" — and the TCP server dies too, so it can't be probed.
 *
 * The watchdog runs on its OWN thread. The main loop calls vb_watchdog_beat()
 * at the top of every iteration with its current phase + PC; the watchdog
 * samples that heartbeat and, if it stops advancing for the stall threshold,
 * prints a one-shot diagnostic banner to stderr (a fault report, like
 * vb_stub_abort — NOT debug spew) naming the phase and PC it died in, then
 * repeats periodically while still hung. The live snapshot is also queryable
 * over TCP (`watchdog` command) while the main thread is alive.
 *
 * This is observe-only: it never touches emulator state, so it cannot perturb
 * the bug it is there to catch.
 */
#ifndef VB_WATCHDOG_H
#define VB_WATCHDOG_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Main-loop phases, reported in the hang banner / TCP status. */
enum {
    VB_WD_INIT     = 0,
    VB_WD_IDLE     = 1,  /* HALT idle tick (sleeping, expected to beat) */
    VB_WD_DISPATCH = 2,  /* inside vb_dispatch (recompiled CPU) */
    VB_WD_TICK     = 3,  /* device ticks (timer/vip/vsu) */
    VB_WD_PRESENT  = 4,  /* SDL render/present + frame pacing */
    VB_WD_POLL     = 5   /* TCP debug-server poll / input */
};

/* Start/stop the watchdog thread. Safe to call once at startup / shutdown. */
void vb_watchdog_start(void);
void vb_watchdog_stop(void);

/* Record a heartbeat from the main loop (cheap; just stores atomics). */
void vb_watchdog_beat(int phase, uint32_t pc, uint64_t cycles, uint64_t frame);

/* Snapshot for the TCP `watchdog` command. `stalled_ms` is how long the
 * heartbeat has been frozen as of now (0 if advancing). Any out-param may be
 * NULL. */
void vb_watchdog_status(int* phase, uint32_t* pc, uint64_t* cycles,
                        uint64_t* frame, uint64_t* beats, int* stalled_ms);

/* Human-readable phase name for a VB_WD_* value. */
const char* vb_watchdog_phase_name(int phase);

#ifdef __cplusplus
}
#endif

#endif /* VB_WATCHDOG_H */
