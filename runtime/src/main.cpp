/* main.cpp — vb-runtime entry point.
 *
 * Phase 1 skeleton:
 *   - parse args
 *   - optionally load a ROM and wire bus pointers
 *   - start the TCP debug server
 *   - service it in a non-blocking loop until "quit" arrives
 *
 * Phase 3+ replaces the idle loop with a real frame loop that calls
 * vb_dispatch() at the reset vector and runs to the next yield boundary.
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "cpu_state.h"
#include "debug_server.h"
#include "interrupts.h"
#include "memory.h"
#include "ring_frame.h"
#include "timer.h"
#include "vip.h"
#include "wtrace.h"
#include "fntrace.h"

#ifndef VB_DEFAULT_DEBUG_PORT
#define VB_DEFAULT_DEBUG_PORT 4390
#endif

#ifndef VB_DEFAULT_WINDOW_TITLE
#define VB_DEFAULT_WINDOW_TITLE "vbrecomp"
#endif

static void print_help(const char* argv0) {
    std::printf(
        "%s — Virtual Boy static-recomp runtime (Phase 1 skeleton)\n"
        "\n"
        "Usage: %s [--rom PATH] [--port N]\n"
        "\n"
        "Options:\n"
        "  --rom PATH       Load a Virtual Boy ROM into the simulated cart slot.\n"
        "                   Optional in Phase 1; without it, the runtime starts the\n"
        "                   TCP server and idles. With a ROM, the bus is wired but\n"
        "                   dispatch is not (no generated game C linked yet).\n"
        "  --port N         TCP debug port (default: %d).\n"
        "  --help, -h       Show this help and exit.\n"
        "\n"
        "TCP harness:        see TCP.md for the JSON command surface.\n"
        "Constitution:       see CLAUDE.md before making changes.\n",
        VB_DEFAULT_WINDOW_TITLE, argv0, VB_DEFAULT_DEBUG_PORT);
}

int main(int argc, char** argv) {
    int port = VB_DEFAULT_DEBUG_PORT;
    const char* rom_path = nullptr;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--help" || a == "-h")) {
            print_help(argv[0]);
            return 0;
        } else if (a == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (a == "--rom" && i + 1 < argc) {
            rom_path = argv[++i];
        } else {
            std::fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]);
            return 2;
        }
    }

    CPUState cpu;
    std::memset(&cpu, 0, sizeof(cpu));

    if (rom_path) {
        int rc = vb_memory_init(rom_path);
        if (rc != 0) {
            std::fprintf(stderr,
                "vb-runtime: failed to load ROM '%s' (rc=%d)\n", rom_path, rc);
            return 3;
        }
        cpu.read8 = vb_read8;
        cpu.read16 = vb_read16;
        cpu.read32 = vb_read32;
        cpu.write8 = vb_write8;
        cpu.write16 = vb_write16;
        cpu.write32 = vb_write32;
        vb_cpu_reset(&cpu);
        // Seed r31 with the dispatch sentinel ONCE here, not on every
        // vb_dispatch entry. The cart's reset trampoline reaches its
        // entry point via JR (no JAL) so r31 is never updated from this
        // initial value until the first JAL fires. The cart then saves
        // r31 onto its stack at function prologues so subsequent JMP r31
        // unwinds correctly all the way back to the sentinel. Setting
        // this in vb_dispatch instead would clobber the cart's live r31
        // on every yielded resume and break the cart's main loop.
        cpu.gpr[31] = 0xDEAD0000u;
        std::printf("vb-runtime: loaded ROM %s (%u bytes), reset PC 0x%08X\n",
                    rom_path, vb_rom_size(), VB_RESET_VECTOR);
    } else {
        std::printf("vb-runtime: no --rom provided; TCP-only idle mode\n");
    }

    // VIP/VSU/input/IRQ/timer are initialised by vb_memory_init when a
    // ROM is loaded. In idle mode (no ROM), they stay quiescent.
    vb_ring_frame_init();
    vb_wtrace_init();
    vb_wtrace_set_active_cpu(&cpu);
    vb_fntrace_init();
    vb_fntrace_set_active_cpu(&cpu);

    if (vb_debug_server_start(port, &cpu) != 0) {
        std::fprintf(stderr,
            "vb-runtime: failed to bind TCP port %d\n", port);
        vb_memory_shutdown();
        vb_ring_frame_shutdown();
        return 4;
    }

    std::printf("vb-runtime: listening on 127.0.0.1:%d\n", port);
    std::fflush(stdout);

    using namespace std::chrono_literals;

    if (rom_path) {
        std::printf("vb-runtime: dispatching to reset vector "
                    "0x%08X\n", VB_RESET_VECTOR);
        std::fflush(stdout);
    }

    // P4-A main loop: alternate between recompiled-code dispatch
    // and TCP polling, and tick device emulation (timer, VIP) by the
    // V810 cycle delta consumed each pass. Between passes, check the
    // IRQ controller and route to an interrupt vector when one is
    // pending and accepted.
    //
    // Cycle accounting: the BB-leader yield-budget decrements
    // cpu.step_budget once per basic block. Residue after a yield
    // therefore measures BBs executed; we scale by ~3 to estimate
    // V810 cycles (average BB ≈ 3 instructions × ~1 cycle each).
    // This is coarse but sufficient for the timer/IRQ work — the
    // VIP state machine (P4-B) will demand cycle accuracy and the
    // residue scaling will be revisited then.
    constexpr uint64_t STEP_BUDGET     = 250000;
    constexpr uint32_t CYCLES_PER_BB   = 3;
    /* While halted, advance device emulation in big chunks. 20MHz
     * CPU × 20ms wallclock per main-loop iteration ≈ 400k cycles.
     * A frame is ~397k cycles, so each iteration covers about one
     * frame — fast enough for ISR-driven title-screen setup to
     * converge in seconds rather than minutes, without blocking
     * TCP polling longer than the next main-loop pass. The 1ms
     * sleep keeps the OS scheduler from pegging a core. */
    constexpr uint64_t IDLE_TICK_CYCLES = 20000;
    bool dispatched_once = false;
    uint32_t dispatch_pc = cpu.pc;
    while (vb_debug_server_poll() == 0) {
        if (!rom_path) {
            std::this_thread::sleep_for(2ms);
            continue;
        }

        // Deliver any pending IRQ before re-entering dispatch.
        // vb_irq_check_and_deliver retargets cpu.pc to the vector
        // when accepted and clears cpu.halted; the next dispatch
        // call will start executing the ISR.
        if (vb_irq_check_and_deliver(&cpu)) {
            dispatch_pc = cpu.pc;
        }

        if (cpu.halted) {
            // Tick devices in idle so the timer / VIP can fire and
            // break a HALT. Match Beetle's wall-clock progression: a
            // single 20µs interval per main-loop pass keeps TCP
            // responsive and the IRQ→ISR→RETI loop converging.
            cpu.cycles += IDLE_TICK_CYCLES;
            vb_timer_tick((uint32_t)IDLE_TICK_CYCLES);
            vb_vip_tick(IDLE_TICK_CYCLES);
            std::this_thread::sleep_for(1ms);
            continue;
        }

        cpu.step_budget = STEP_BUDGET;
        cpu.yielded = 0;
        vb_dispatch(&cpu, dispatch_pc);

        const uint64_t bbs_run  = STEP_BUDGET - cpu.step_budget;
        const uint64_t cyc_delta = bbs_run * CYCLES_PER_BB;
        cpu.cycles += cyc_delta;
        vb_timer_tick((uint32_t)cyc_delta);
        vb_vip_tick(cyc_delta);

        if (cpu.yielded) {
            // Resume from wherever the cart left off next tick.
            dispatch_pc = cpu.pc;
            if (!dispatched_once) {
                std::printf("vb-runtime: first yield at pc=0x%08X "
                            "(cart is running)\n", cpu.pc);
                std::fflush(stdout);
                dispatched_once = true;
            }
        } else if (cpu.halted) {
            // HALT instruction; idle loop above will tick devices
            // until an IRQ wakes us. cpu.pc points just past the
            // HALT; on resume, dispatch_pc will be set by the IRQ
            // check above.
            dispatch_pc = cpu.pc;
            std::printf("vb-runtime: HALT at pc=0x%08X (waiting for IRQ)\n",
                        cpu.pc);
            std::fflush(stdout);
        } else {
            // Top-level JMP r31 — the cart "returned from main". VB
            // carts often boot, set up VIP, and JMP r31 expecting an
            // OS to take over; on real hardware the CPU would fetch
            // garbage. Our recomp catches this via the DEAD0000
            // sentinel and treats it as a HALT — the IRQ-driven
            // main work then runs entirely from ISRs (VIP frame
            // events) until input or another event extends the
            // cart's state. Without this the runtime would idle on
            // TCP and stop dispatching forever.
            //
            // Subsequent ISRs entering and RETI'ing back here cycle
            // through this branch — so the print is gated to fire
            // only on the first entry.
            static bool s_sentinel_logged = false;
            if (!s_sentinel_logged) {
                std::printf("vb-runtime: top-level JMP r31 to sentinel "
                            "(pc=0x%08X) — entering HALT-equivalent "
                            "idle; ISRs will continue to dispatch on "
                            "IRQ delivery\n", cpu.pc);
                std::fflush(stdout);
                s_sentinel_logged = true;
            }
            cpu.halted = 1;
            dispatch_pc = cpu.pc;
        }
    }

    vb_debug_server_stop();
    vb_memory_shutdown();
    vb_ring_frame_shutdown();
    vb_wtrace_shutdown();
    vb_fntrace_shutdown();
    return 0;
}
