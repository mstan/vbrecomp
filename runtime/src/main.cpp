/* main.cpp — vb-runtime entry point.
 *
 * Drives the recompiled cart in a step-budget dispatch loop, ticks
 * device emulation (timer + VIP) by the cycle delta consumed each
 * pass, services the TCP debug server, and (when SDL2 is available)
 * presents the latest VIP framebuffer at 50.27 Hz with keyboard
 * input feeding vb_input_set_pad.
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "asset_pack.h"
#include "recolor.h"
#include "cpu_state.h"
#include "debug_server.h"
#include "input.h"
#include "interrupts.h"
#include "memory.h"
#include "ring_frame.h"
#include "timer.h"
#include "vip.h"
#include "vsu.h"
#include "watchdog.h"
#include "wtrace.h"
#include "fntrace.h"

#if VB_RUNTIME_HAVE_SDL
#  include <SDL.h>
#endif

#ifndef VB_DEFAULT_DEBUG_PORT
#define VB_DEFAULT_DEBUG_PORT 4390
#endif

#ifndef VB_DEFAULT_WINDOW_TITLE
#define VB_DEFAULT_WINDOW_TITLE "vbrecomp"
#endif

static void print_help(const char* argv0) {
    std::printf(
        "%s — Virtual Boy static-recomp runtime\n"
        "\n"
        "Usage: %s [--rom PATH] [--port N] [--headless] [--stereo]\n"
        "\n"
        "Options:\n"
        "  --rom PATH       Load a Virtual Boy ROM into the simulated cart slot.\n"
        "                   Without it, the runtime starts the TCP server and idles.\n"
        "  --port N         TCP debug port (default: %d).\n"
        "  --headless       Do not open an SDL window. TCP-only.\n"
        "  --stereo         Show both eyes stacked vertically (L top / R bottom)\n"
        "                   instead of the single-eye default.\n"
        "  --help, -h       Show this help and exit.\n"
        "\n"
        "Keyboard map (when the SDL window has focus):\n"
        "  Arrows .......... Left D-pad\n"
        "  W A S D ......... Right D-pad\n"
        "  X / Z ........... A / B\n"
        "  Q / E ........... L / R triggers\n"
        "  Enter / RShift .. Start / Select\n"
        "  TAB ............. Turbo (skip 50.27 Hz pacing)\n"
        "  F11 / Alt+Enter . Toggle fullscreen\n"
        "  Esc ............. Quit\n"
        "\n"
        "Gamepad (SDL game controller, player 1):\n"
        "  D-pad / L-stick . Left D-pad      R-stick ......... Right D-pad\n"
        "  A / B button .... A / B           LB / RB ......... L / R triggers\n"
        "  Start / Back .... Start / Select\n"
        "\n"
        "TCP harness:        see TCP.md for the JSON command surface.\n"
        "Constitution:       see CLAUDE.md before making changes.\n",
        VB_DEFAULT_WINDOW_TITLE, argv0, VB_DEFAULT_DEBUG_PORT);
}

#if VB_RUNTIME_HAVE_SDL
/* VB native single-eye geometry. Window defaults to single-eye
 * (eye 0 / left); --stereo opt-in stacks both eyes vertically. */
static constexpr int VB_RT_EYE_W = 384;
static constexpr int VB_RT_EYE_H = 224;
static constexpr double VB_RT_FRAME_HZ = 50.27;
static constexpr int VB_RT_WIN_SCALE = 2;   /* legibility default */

static uint16_t pad_from_keyboard(void) {
    /* vb-runtime's pad word is ACTIVE-HIGH (a set bit = pressed).
     * This is opposite to vb-beetle's active-low convention; the
     * V810 input register synthesises the active-low view at read
     * time inside input.c. */
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    uint16_t pad = 0;
    if (!keys) return pad;
    if (keys[SDL_SCANCODE_UP])     pad |= VB_PAD_LUP;
    if (keys[SDL_SCANCODE_DOWN])   pad |= VB_PAD_LDOWN;
    if (keys[SDL_SCANCODE_LEFT])   pad |= VB_PAD_LLEFT;
    if (keys[SDL_SCANCODE_RIGHT])  pad |= VB_PAD_LRIGHT;
    if (keys[SDL_SCANCODE_W])      pad |= VB_PAD_RUP;
    if (keys[SDL_SCANCODE_S])      pad |= VB_PAD_RDOWN;
    if (keys[SDL_SCANCODE_A])      pad |= VB_PAD_RLEFT;
    if (keys[SDL_SCANCODE_D])      pad |= VB_PAD_RRIGHT;
    if (keys[SDL_SCANCODE_X])      pad |= VB_PAD_A;
    if (keys[SDL_SCANCODE_Z])      pad |= VB_PAD_B;
    if (keys[SDL_SCANCODE_Q])      pad |= VB_PAD_LT;
    if (keys[SDL_SCANCODE_E])      pad |= VB_PAD_RT;
    if (keys[SDL_SCANCODE_RETURN]) pad |= VB_PAD_START;
    if (keys[SDL_SCANCODE_RSHIFT]) pad |= VB_PAD_SELECT;
    return pad;
}
#endif  /* VB_RUNTIME_HAVE_SDL */

#if VB_RUNTIME_HAVE_SDL
/* Player-1 game controller, opened lazily and tracked across hotplug.
 * SDL_GameController is cross-platform (XInput/DInput on Windows, IOKit
 * on macOS, evdev on Linux), so this replaces the old Windows-only
 * XInput path with one mapping that works everywhere. */
static SDL_GameController* s_pad = nullptr;

/* Open the first attached controller, if any and none is open yet. */
static void gamepad_open_first(void) {
    if (s_pad) return;
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (SDL_IsGameController(i)) {
            s_pad = SDL_GameControllerOpen(i);
            if (s_pad) return;
        }
    }
}

/* React to SDL_CONTROLLERDEVICEADDED / REMOVED so hotplug works. */
static void gamepad_handle_device_event(const SDL_Event& ev) {
    if (ev.type == SDL_CONTROLLERDEVICEADDED) {
        if (!s_pad) s_pad = SDL_GameControllerOpen(ev.cdevice.which);
    } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
        if (s_pad && ev.cdevice.which ==
                SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(s_pad))) {
            SDL_GameControllerClose(s_pad);
            s_pad = nullptr;
            gamepad_open_first();   /* fall back to another pad if present */
        }
    }
}

/* Read player 1, returning the same active-high VB pad word as
 * pad_from_keyboard. Mapping:
 *   D-pad + left stick  -> Left D-pad   (left thumb)
 *   Right stick         -> Right D-pad  (right thumb)
 *   A / B               -> VB A / VB B
 *   LB / RB             -> L / R triggers
 *   Start / Back        -> Start / Select
 * Returns 0 with *out_connected=false when no controller is open. */
static uint16_t pad_from_gamecontroller(bool* out_connected) {
    if (!s_pad) {
        if (out_connected) *out_connected = false;
        return 0;
    }
    if (out_connected) *out_connected = true;
    SDL_GameController* c = s_pad;
    uint16_t pad = 0;

    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_UP))    pad |= VB_PAD_LUP;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_DOWN))  pad |= VB_PAD_LDOWN;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_LEFT))  pad |= VB_PAD_LLEFT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_DPAD_RIGHT)) pad |= VB_PAD_LRIGHT;

    /* Sticks. SDL axes are -32768..32767 with +Y pointing DOWN. */
    constexpr Sint16 DZ = 12000;
    const Sint16 lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
    const Sint16 ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY);
    if (ly < -DZ) pad |= VB_PAD_LUP;
    if (ly >  DZ) pad |= VB_PAD_LDOWN;
    if (lx < -DZ) pad |= VB_PAD_LLEFT;
    if (lx >  DZ) pad |= VB_PAD_LRIGHT;

    const Sint16 rx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTX);
    const Sint16 ry = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_RIGHTY);
    if (ry < -DZ) pad |= VB_PAD_RUP;
    if (ry >  DZ) pad |= VB_PAD_RDOWN;
    if (rx < -DZ) pad |= VB_PAD_RLEFT;
    if (rx >  DZ) pad |= VB_PAD_RRIGHT;

    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_A))             pad |= VB_PAD_A;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_B))             pad |= VB_PAD_B;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_LEFTSHOULDER))  pad |= VB_PAD_LT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER)) pad |= VB_PAD_RT;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_START))         pad |= VB_PAD_START;
    if (SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_BACK))          pad |= VB_PAD_SELECT;
    return pad;
}
#endif  /* VB_RUNTIME_HAVE_SDL */

#if VB_RUNTIME_HAVE_SDL
/* SDL audio callback. Runs on SDL's audio thread; pulls the VSU
 * ring buffer into the device buffer. `vb_vsu_pull_samples` pads
 * with silence when the producer is starved, so brief stalls
 * (TCP-blocked main loop) just produce a momentary quiet rather
 * than a buzzing underrun.
 *
 * `stream` is byte-addressed; we want int16 stereo frames so each
 * SDL "byte" pair is one int16 channel sample. The audio spec
 * below sets `format = AUDIO_S16SYS`, `channels = 2`. */
static void vb_sdl_audio_cb(void* /*ud*/, Uint8* stream, int len) {
    const size_t n_frames = (size_t)len / (2 * sizeof(int16_t));
    vb_vsu_pull_samples((int16_t*)stream, n_frames);
}
#endif

int main(int argc, char** argv) {
    int port = VB_DEFAULT_DEBUG_PORT;
    const char* rom_path = nullptr;
    bool headless = false;
    bool stereo = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if ((a == "--help" || a == "-h")) {
            print_help(argv[0]);
            return 0;
        } else if (a == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (a == "--rom" && i + 1 < argc) {
            rom_path = argv[++i];
        } else if (a == "--headless") {
            headless = true;
        } else if (a == "--stereo" || a == "--dual-eye") {
            stereo = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]);
            return 2;
        }
    }

#if !VB_RUNTIME_HAVE_SDL
    /* The build doesn't have SDL2; window is impossible regardless of
     * the user's wishes. Treat as headless for the rest of main(). */
    headless = true;
#endif

    CPUState cpu;
    std::memset(&cpu, 0, sizeof(cpu));

    if (rom_path) {
        int rc = vb_memory_init(rom_path);
        if (rc != 0) {
            std::fprintf(stderr,
                "vb-runtime: failed to load ROM '%s' (rc=%d)\n", rom_path, rc);
            return 3;
        }
        /* CRC32-verify the loaded cart against the value baked in by
         * the recompiler. A mismatch means the user pointed --rom at
         * something other than the cart this binary was built for —
         * running on would produce garbage (the recompiled C does the
         * original cart's CPU work on the new cart's data). */
        uint32_t expected = vb_game_expected_crc32();
        if (expected != 0) {
            const uint8_t* rom_bytes = vb_rom_data();
            uint32_t rom_n = vb_rom_size();
            /* IEEE 802.3 CRC-32 reflected; matches zlib.crc32 and
             * Python's binascii.crc32. Compact table-free shift loop —
             * one-shot at boot, perf doesn't matter. */
            uint32_t crc = 0xFFFFFFFFu;
            for (uint32_t i = 0; i < rom_n; ++i) {
                crc ^= rom_bytes[i];
                for (int b = 0; b < 8; ++b)
                    crc = (crc >> 1) ^ (0xEDB88320u & -(int32_t)(crc & 1));
            }
            crc ^= 0xFFFFFFFFu;
            if (crc != expected) {
                std::fprintf(stderr,
                    "vb-runtime: ROM CRC32 mismatch.\n"
                    "  expected: 0x%08X (built for this cart)\n"
                    "  actual:   0x%08X (provided '%s')\n"
                    "The recompiled C is specific to one cart dump — "
                    "running against a different ROM file produces "
                    "garbage. Provide the correct .vb file via --rom.\n",
                    expected, crc, rom_path);
                vb_memory_shutdown();
                return 5;
            }
            std::printf("vb-runtime: CRC32 OK (0x%08X)\n", crc);
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

#if VB_RUNTIME_HAVE_SDL
    SDL_Window*       win = nullptr;
    SDL_Renderer*     ren = nullptr;
    SDL_Texture*      tex = nullptr;
    SDL_AudioDeviceID aud = 0;
    Uint64            sdl_freq = 0;
    Uint64            sdl_period = 0;
    Uint64            sdl_deadline = 0;
    const int         tex_w = VB_RT_EYE_W;
    const int         tex_h = stereo ? VB_RT_EYE_H * 2 : VB_RT_EYE_H;
    uint32_t          tex_pixels[VB_RT_EYE_W * VB_RT_EYE_H * 2];

    if (!headless) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO
                     | SDL_INIT_GAMECONTROLLER) != 0) {
            std::fprintf(stderr, "vb-runtime: SDL_Init failed: %s — "
                                 "falling back to headless\n",
                         SDL_GetError());
            headless = true;
        } else {
            gamepad_open_first();
        }
    }
    if (!headless) {
        win = SDL_CreateWindow(
            VB_DEFAULT_WINDOW_TITLE,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            tex_w * VB_RT_WIN_SCALE, tex_h * VB_RT_WIN_SCALE,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!win) {
            std::fprintf(stderr, "vb-runtime: SDL_CreateWindow failed: %s\n",
                         SDL_GetError());
            headless = true;
        }
    }
    if (!headless) {
#ifdef _WIN32
        /* Preserved from the Windows build; on macOS/Linux let SDL pick its
         * native backend (Metal on Apple Silicon). */
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
#endif
        /* PRESENTVSYNC aligns presents to the display refresh to avoid
         * scroll tearing; the manual pacer below still bounds the rate.
         * Fall back progressively if a driver can't provide vsync/accel. */
        ren = SDL_CreateRenderer(win, -1,
                                 SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
        if (!ren) {
            std::fprintf(stderr, "vb-runtime: SDL_CreateRenderer failed: %s\n",
                         SDL_GetError());
            headless = true;
        }
    }
    if (!headless) {
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING,
                                tex_w, tex_h);
        if (!tex) {
            std::fprintf(stderr, "vb-runtime: SDL_CreateTexture failed: %s\n",
                         SDL_GetError());
            headless = true;
        }
    }
    if (!headless) {
        sdl_freq = SDL_GetPerformanceFrequency();
        const double frame_ms = 1000.0 / VB_RT_FRAME_HZ;
        sdl_period = (Uint64)((double)sdl_freq * (frame_ms / 1000.0));
        std::printf("vb-runtime: SDL window %dx%d at %.2f Hz (%s)\n",
                    tex_w * VB_RT_WIN_SCALE, tex_h * VB_RT_WIN_SCALE,
                    VB_RT_FRAME_HZ,
                    stereo ? "L eye top / R eye bottom"
                           : "single eye (--stereo for both)");
        std::fflush(stdout);

        /* Audio device. Failure is non-fatal — the window keeps
         * running with the cart's writes accumulating into the VSU
         * ring but no sound out. */
        SDL_AudioSpec want;
        std::memset(&want, 0, sizeof(want));
        want.freq     = VSU_OUTPUT_HZ;
        want.format   = AUDIO_S16SYS;
        want.channels = 2;
        want.samples  = 1024;            /* ~23 ms @ 44.1 kHz */
        want.callback = vb_sdl_audio_cb;
        SDL_AudioSpec have;
        aud = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (aud == 0) {
            std::fprintf(stderr,
                "vb-runtime: SDL_OpenAudioDevice failed: %s "
                "(continuing without audio)\n", SDL_GetError());
        } else {
            SDL_PauseAudioDevice(aud, 0);
            std::printf("vb-runtime: SDL audio at %d Hz, %d-frame "
                        "buffer\n", have.freq, have.samples);
            std::fflush(stdout);
        }
    }
#endif  /* VB_RUNTIME_HAVE_SDL */

    if (rom_path) {
        std::printf("vb-runtime: dispatching to reset vector "
                    "0x%08X\n", VB_RESET_VECTOR);
        std::fflush(stdout);
    }

    /* VB CPU 20 MHz / 50.27 Hz frame ≈ 397 853 cycles. The dispatch
     * loop advances at ~750 000 cycles per pass when not halted, so
     * "one frame ready to present" is the natural cadence; in halted
     * idle it takes ~20 passes (IDLE_TICK_CYCLES) to accumulate one
     * frame's worth, which still leaves TCP/SDL polls responsive. */
    constexpr uint64_t VB_CYCLES_PER_FRAME = 397853;
    uint64_t last_present_cycles = 0;
    bool sdl_quit = false;

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
    uint64_t present_count = 0;
    /* Always-on hang watchdog (separate thread). Started here, from the main
     * thread, so it can capture this thread's stack on a freeze. */
    vb_watchdog_start();
    while (vb_debug_server_poll() == 0 && !sdl_quit) {
        vb_watchdog_beat(VB_WD_POLL, dispatch_pc, cpu.cycles, present_count);
#if VB_RUNTIME_HAVE_SDL
        if (!headless) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { sdl_quit = true; break; }
                if (ev.type == SDL_CONTROLLERDEVICEADDED ||
                    ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                    gamepad_handle_device_event(ev);
                }
                if (ev.type == SDL_KEYDOWN) {
                    if (ev.key.keysym.sym == SDLK_ESCAPE) {
                        sdl_quit = true;
                        break;
                    }
                    /* Fullscreen toggle: F11, Alt+Enter, or Cmd/Ctrl+F.
                     * FULLSCREEN_DESKTOP keeps the desktop resolution; the
                     * present loop's letterbox math scales the eye image. */
                    const Uint16 mod = ev.key.keysym.mod;
                    if (ev.key.keysym.sym == SDLK_F11 ||
                        (ev.key.keysym.sym == SDLK_RETURN && (mod & KMOD_ALT)) ||
                        (ev.key.keysym.sym == SDLK_f && (mod & (KMOD_GUI | KMOD_CTRL)))) {
                        Uint32 is_fs = SDL_GetWindowFlags(win) &
                                       SDL_WINDOW_FULLSCREEN_DESKTOP;
                        SDL_SetWindowFullscreen(win,
                            is_fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                    }
                }
            }
            if (sdl_quit) break;
        }
#endif
        /* Compose the pad. With a window open, the keyboard is the
         * authority and the game controller OR's in; this clobbers TCP
         * press/set_input every frame. Controller support requires SDL,
         * so a --headless run is driven purely by TCP commands. */
#if VB_RUNTIME_HAVE_SDL
        if (!headless) {
            uint16_t controller_pad = pad_from_gamecontroller(nullptr);
            vb_input_set_pad((uint16_t)(pad_from_keyboard() | controller_pad));
        }
#endif

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
            vb_vsu_tick(IDLE_TICK_CYCLES);
            std::this_thread::sleep_for(1ms);
            continue;
        }

        cpu.step_budget = STEP_BUDGET;
        cpu.yielded = 0;
        vb_watchdog_beat(VB_WD_DISPATCH, dispatch_pc, cpu.cycles, present_count);
        vb_dispatch(&cpu, dispatch_pc);

        const uint64_t bbs_run  = STEP_BUDGET - cpu.step_budget;
        const uint64_t cyc_delta = bbs_run * CYCLES_PER_BB;
        cpu.cycles += cyc_delta;
        vb_timer_tick((uint32_t)cyc_delta);
        vb_vip_tick(cyc_delta);
        vb_vsu_tick(cyc_delta);

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

#if VB_RUNTIME_HAVE_SDL
        if (!headless && (cpu.cycles - last_present_cycles) >= VB_CYCLES_PER_FRAME) {
            last_present_cycles = cpu.cycles;
            vb_watchdog_beat(VB_WD_PRESENT, dispatch_pc, cpu.cycles, ++present_count);

            /* Opt-in full-screen recolor uses the recolored present path;
             * otherwise the faithful render. */
            const bool recolor = vb_recolor_active();
            if (recolor) {
                vb_vip_render_framebuffer_recolored(0, &tex_pixels[0]);
                if (stereo) {
                    vb_vip_render_framebuffer_recolored(1,
                        &tex_pixels[VB_RT_EYE_H * VB_RT_EYE_W]);
                }
            } else {
                vb_vip_render_framebuffer(0, &tex_pixels[0]);
                if (stereo) {
                    vb_vip_render_framebuffer(1,
                        &tex_pixels[VB_RT_EYE_H * VB_RT_EYE_W]);
                }
            }

            /* Opt-in override overlays (experiment): composite the colored
             * RGBA replacements over the faithful framebuffer, per eye, using
             * the draw-list slot matching the displayed buffer. No-op when
             * VBRECOMP_OVERRIDES is unset (empty list) ⇒ unchanged output. */
            if (vb_overrides_active()) {
                int slot = vb_vip_display_fb() & 1;
                vb_overlay_composite(&tex_pixels[0],
                                     VB_RT_EYE_W, VB_RT_EYE_H, 0, slot);
                if (stereo) {
                    vb_overlay_composite(&tex_pixels[VB_RT_EYE_H * VB_RT_EYE_W],
                                         VB_RT_EYE_W, VB_RT_EYE_H, 1, slot);
                }
            }

            SDL_UpdateTexture(tex, nullptr, tex_pixels,
                              tex_w * (int)sizeof(uint32_t));

            int win_w = 0, win_h = 0;
            SDL_GetRendererOutputSize(ren, &win_w, &win_h);
            double sx = (double)win_w / tex_w;
            double sy = (double)win_h / tex_h;
            double s  = (sx < sy) ? sx : sy;
            SDL_Rect dst;
            dst.w = (int)(tex_w * s);
            dst.h = (int)(tex_h * s);
            dst.x = (win_w - dst.w) / 2;
            dst.y = (win_h - dst.h) / 2;

            SDL_SetRenderDrawColor(ren, 0, 0, 0, 0xFF);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, tex, nullptr, &dst);
            SDL_RenderPresent(ren);

            /* Pace to VB_RT_FRAME_HZ — TAB skips the wait (turbo). */
            const Uint8* keys = SDL_GetKeyboardState(nullptr);
            const bool turbo = keys && keys[SDL_SCANCODE_TAB];
            Uint64 now = SDL_GetPerformanceCounter();
            if (sdl_deadline == 0 || now >= sdl_deadline + sdl_period * 3) {
                /* Either first present or we fell badly behind; resync. */
                sdl_deadline = now + sdl_period;
            } else if (!turbo) {
                /* Read the perf counter ONCE per iteration. The old code read
                 * it in the while-condition AND again in `left = deadline -
                 * counter`; if the counter crossed the deadline between those
                 * two reads, the unsigned subtraction underflowed to ~1.8e19,
                 * giving SDL_Delay() a ~49-day argument — a permanent freeze
                 * ("Not Responding"). Idle windowed runs present every frame,
                 * so over time the race was inevitable. (Same bug class as
                 * psxrecomp's freeze_heartbeat note.) */
                for (;;) {
                    Uint64 n2 = SDL_GetPerformanceCounter();
                    if (n2 >= sdl_deadline) break;          /* n2 < deadline ⇒ no underflow */
                    Uint64 left = sdl_deadline - n2;
                    Uint64 ms = (left * 1000) / sdl_freq;
                    if (ms >= 2) SDL_Delay((Uint32)(ms - 1));
                    else break;
                }
                while (SDL_GetPerformanceCounter() < sdl_deadline) { /* spin remainder */ }
                sdl_deadline += sdl_period;
            } else {
                sdl_deadline += sdl_period;
            }
        }
#endif
    }

    vb_watchdog_stop();

#if VB_RUNTIME_HAVE_SDL
    if (s_pad) { SDL_GameControllerClose(s_pad); s_pad = nullptr; }
    if (aud) { SDL_PauseAudioDevice(aud, 1); SDL_CloseAudioDevice(aud); }
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    if (!headless) SDL_Quit();
#endif

    vb_debug_server_stop();
    vb_memory_shutdown();
    vb_ring_frame_shutdown();
    vb_wtrace_shutdown();
    vb_fntrace_shutdown();
    return 0;
}
