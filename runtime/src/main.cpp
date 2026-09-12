/* main.cpp — vb-runtime entry point.
 *
 * Drives the recompiled cart in a step-budget dispatch loop, ticks
 * device emulation (timer + VIP) by the cycle delta consumed each
 * pass, services the TCP debug server, and (when SDL2 is available)
 * presents the latest VIP framebuffer at 50.27 Hz with keyboard
 * input feeding vb_input_set_pad.
 */
#include <chrono>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "asset_pack.h"
#include "recolor.h"
#include "renderer.h"
#include "viewport.h"
#include "mod_runtime.h"
#include "host.h"
#include "png_write.h"
#include "cpu_state.h"
#include "v810_interpreter.h"
#include "debug_server.h"
#include "input.h"
#include "interrupts.h"
#include "memory.h"
#include "rom_patch.h"
#include "stub_abort.h"
#include "ring_frame.h"
#include "timer.h"
#include "vip.h"
#include "vsu.h"
#include "watchdog.h"
#include "wtrace.h"
#include "fntrace.h"

#if VB_RUNTIME_HAVE_SDL
#  include <SDL.h>
#  include "host_controller.h"
#endif
#if defined(RECOMP_LAUNCHER) && VB_RUNTIME_HAVE_SDL
#  include "recomp_runtime_ui.h"
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
        "                   Game builds can select or reuse a ROM in the launcher.\n"
        "  --port N         TCP debug port (default: %d).\n"
        "  --headless       Do not open an SDL window. TCP-only.\n"
        "  --stereo         Show both eyes stacked vertically (L top / R bottom)\n"
        "                   instead of the single-eye default.\n"
        "  --paused         Start paused (requires debug tools).\n"
        "  --execution MODE hybrid (default), native, or interpreter.\n"
        "  --save PATH      Cartridge save (64KiB, saved on clean exit).\n"
        "  --no-save        Disable cartridge save loading/writing.\n"
        "  --launcher       Always open the game launcher.\n"
        "  --no-launcher    Use saved settings and start directly.\n"
        "  --config PATH    Settings file (default: beside executable).\n"
        "  --mods-dir PATH  Package catalog and selections directory.\n"
        "  --install-mod P  Install a .vbmod archive.\n"
        "  --enable-mod P:F / --disable-mod P:F  Select package feature.\n"
        "  --set-mod-option P:F:O=V  Set a package feature option.\n"
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
        "  Esc ............. Settings menu (UI builds); quit otherwise\n"
        "\n"
        "Gamepad (SDL game controller, player 1):\n"
        "  D-pad ........... Left D-pad      R-stick ......... Right D-pad\n"
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

static std::atomic<int> s_runtime_audio_volume{100};
struct VbHostCapture { const uint32_t* pixels; int width, height; };
static int vb_capture_host(const char* path, void* context) {
    const auto* frame = static_cast<VbHostCapture*>(context);
    return vb_write_png_32bpp(path, frame->width, frame->height, frame->pixels);
}

#if defined(RECOMP_LAUNCHER)
struct VbRuntimeUiContext {
    SDL_Window* window;
    SDL_Texture* texture;
    SDL_AudioDeviceID audio;
    int window_scale;
    int linear_filter;
    int audio_enabled;
    int volume;
};
static RecompRuntimeUi* s_runtime_ui = nullptr;
static bool vb_ui_mod_identity(const RecompRuntimeUiItem* item, std::string& package, std::string& feature) {
    const std::string key = item->key;
    if (key.rfind("mod:", 0) != 0) return false;
    const auto split = key.find(':', 4);
    if (split == std::string::npos) return false;
    package = key.substr(4, split - 4); feature = key.substr(split + 1);
    return true;
}
static int vb_ui_mod_get(const std::string& package, const std::string& feature, int* out) {
    const auto* provider = vb_mod_runtime_launcher_provider_c();
    if (!provider) return 0;
    for (int i = 0; i < provider->feature_count(provider->ctx); ++i) {
        RecompLauncherCModFeature f = {};
        if (provider->feature_get(provider->ctx, i, &f) && package == f.package_id && feature == f.id) {
            *out = f.enabled; return 1;
        }
    }
    return 0;
}
static int vb_runtime_ui_get(void* opaque, const RecompRuntimeUiItem* item, int* out) {
    auto* c = static_cast<VbRuntimeUiContext*>(opaque);
    if (!c || !item || !out) return 0;
    std::string package, feature;
    if (vb_ui_mod_identity(item, package, feature)) return vb_ui_mod_get(package, feature, out);
    if (std::strcmp(item->key, RECOMP_RUNTIME_UI_KEY_FULLSCREEN) == 0) {
        const Uint32 flags = SDL_GetWindowFlags(c->window);
        *out = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP ? 1
             : (flags & SDL_WINDOW_FULLSCREEN) ? 2 : 0;
    } else if (std::strcmp(item->key, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE) == 0) *out = c->window_scale;
    else if (std::strcmp(item->key, RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER) == 0) *out = c->linear_filter;
    else if (std::strcmp(item->key, RECOMP_RUNTIME_UI_KEY_AUDIO) == 0) *out = c->audio_enabled;
    else if (std::strcmp(item->key, RECOMP_RUNTIME_UI_KEY_VOLUME) == 0) *out = c->volume;
    else return 0;
    return 1;
}

static int vb_runtime_ui_set(void* opaque, const RecompRuntimeUiItem* item, int value) {
    auto* c = static_cast<VbRuntimeUiContext*>(opaque);
    if (!c || !item) return 0;
    std::string package, feature;
    if (vb_ui_mod_identity(item, package, feature)) {
        int before = 0;
        if (!vb_ui_mod_get(package, feature, &before)) return 0;
        if (!vb_mod_enable(package.c_str(), feature.c_str(), value)) return 0;
        if (vb_host_commit_mods()) return 1;
        vb_mod_enable(package.c_str(), feature.c_str(), before);
        return 0;
    }
    if (std::strcmp(item->key, RECOMP_RUNTIME_UI_KEY_FULLSCREEN) == 0) {
        Uint32 flag = value == 2 ? SDL_WINDOW_FULLSCREEN
                    : value == 1 ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0;
        if (SDL_SetWindowFullscreen(c->window, flag) != 0) return 0;
    } else if (std::strcmp(item->key, RECOMP_RUNTIME_UI_KEY_WINDOW_SCALE) == 0) {
        c->window_scale = value < 1 ? 1 : value > 6 ? 6 : value;
        int w = 0, h = 0;
        SDL_QueryTexture(c->texture, nullptr, nullptr, &w, &h);
        SDL_SetWindowSize(c->window, w * c->window_scale, h * c->window_scale);
    } else if (std::strcmp(item->key, RECOMP_RUNTIME_UI_KEY_LINEAR_FILTER) == 0) {
        c->linear_filter = value != 0;
#if SDL_VERSION_ATLEAST(2, 0, 12)
        SDL_SetTextureScaleMode(c->texture, c->linear_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#endif
    } else if (std::strcmp(item->key, RECOMP_RUNTIME_UI_KEY_AUDIO) == 0) {
        c->audio_enabled = value != 0;
        if (c->audio) SDL_PauseAudioDevice(c->audio, c->audio_enabled ? 0 : 1);
    } else if (std::strcmp(item->key, RECOMP_RUNTIME_UI_KEY_VOLUME) == 0) {
        c->volume = value < 0 ? 0 : value > 100 ? 100 : value;
        s_runtime_audio_volume = c->volume;
    } else return 0;
    vb_host_config.scale = c->window_scale;
    vb_host_config.filter = c->linear_filter;
    vb_host_config.audio = c->audio_enabled;
    vb_host_config.volume = c->volume;
    const Uint32 flags = SDL_GetWindowFlags(c->window);
    vb_host_config.fullscreen = (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) == SDL_WINDOW_FULLSCREEN_DESKTOP ? 1 : (flags & SDL_WINDOW_FULLSCREEN) ? 2 : 0;
    return 1;
}

static int vb_runtime_ui_action(void*, const RecompRuntimeUiItem* item) {
    if (item && std::strcmp(item->key, RECOMP_RUNTIME_UI_KEY_RESUME) == 0) {
        recomp_runtime_ui_close(s_runtime_ui);
        return 1;
    }
    return 0;
}

static bool vb_runtime_ui_event(const SDL_Event& ev) {
    if (!s_runtime_ui) return false;
    if (ev.type == SDL_KEYDOWN && ev.key.keysym.scancode == SDL_SCANCODE_ESCAPE &&
        !recomp_runtime_ui_is_open(s_runtime_ui)) {
        recomp_runtime_ui_open(s_runtime_ui);
        return true;
    }
    if (ev.type != SDL_KEYDOWN && ev.type != SDL_KEYUP &&
        ev.type != SDL_CONTROLLERBUTTONDOWN && ev.type != SDL_CONTROLLERBUTTONUP)
        return false;
    RecompRuntimeUiInput input;
    bool mapped = true;
    int pressed = ev.type == SDL_KEYDOWN || ev.type == SDL_CONTROLLERBUTTONDOWN;
    int repeat = ev.type == SDL_KEYDOWN ? ev.key.repeat : 0;
    if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
        switch (ev.key.keysym.scancode) {
            case SDL_SCANCODE_ESCAPE: input = RECOMP_RUNTIME_UI_INPUT_BACK; break;
            case SDL_SCANCODE_UP: input = RECOMP_RUNTIME_UI_INPUT_UP; break;
            case SDL_SCANCODE_DOWN: input = RECOMP_RUNTIME_UI_INPUT_DOWN; break;
            case SDL_SCANCODE_LEFT: input = RECOMP_RUNTIME_UI_INPUT_LEFT; break;
            case SDL_SCANCODE_RIGHT: input = RECOMP_RUNTIME_UI_INPUT_RIGHT; break;
            case SDL_SCANCODE_RETURN:
            case SDL_SCANCODE_SPACE: input = RECOMP_RUNTIME_UI_INPUT_ACCEPT; break;
            default: mapped = false; break;
        }
    } else {
        switch (ev.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_GUIDE:
                if (pressed) {
                    if (recomp_runtime_ui_is_open(s_runtime_ui)) recomp_runtime_ui_close(s_runtime_ui);
                    else recomp_runtime_ui_open(s_runtime_ui);
                }
                return true;
            case SDL_CONTROLLER_BUTTON_DPAD_UP: input = RECOMP_RUNTIME_UI_INPUT_UP; break;
            case SDL_CONTROLLER_BUTTON_DPAD_DOWN: input = RECOMP_RUNTIME_UI_INPUT_DOWN; break;
            case SDL_CONTROLLER_BUTTON_DPAD_LEFT: input = RECOMP_RUNTIME_UI_INPUT_LEFT; break;
            case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: input = RECOMP_RUNTIME_UI_INPUT_RIGHT; break;
            case SDL_CONTROLLER_BUTTON_A: input = RECOMP_RUNTIME_UI_INPUT_ACCEPT; break;
            case SDL_CONTROLLER_BUTTON_B: input = RECOMP_RUNTIME_UI_INPUT_BACK; break;
            default: mapped = false; break;
        }
    }
    if (mapped && recomp_runtime_ui_is_open(s_runtime_ui))
        recomp_runtime_ui_handle_input(s_runtime_ui, input, pressed, repeat);
    return recomp_runtime_ui_is_open(s_runtime_ui) || mapped;
}
#endif

static uint16_t pad_from_keyboard(void) {
    /* Hardware pad masks use one bit per pressed button. */
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    uint16_t pad = 0;
    if (!keys) return pad;
    if (vb_host_config.player_source != 1) return 0;
    for (int i = 0; i < 14; ++i) {
        const int key = vb_host_config.keys[i];
        if (key > 0 && key < SDL_NUM_SCANCODES && keys[key]) pad |= vb_host_pad_bits[i];
    }
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
            char guid[40] = {};
            SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i), guid, sizeof(guid));
            if (!vb_host_config.gamepad_guid.empty() && vb_host_config.gamepad_guid != guid) continue;
            s_pad = SDL_GameControllerOpen(i);
            if (s_pad) return;
        }
    }
}

/* React to SDL_CONTROLLERDEVICEADDED / REMOVED so hotplug works. */
static void gamepad_handle_device_event(const SDL_Event& ev) {
    if (ev.type == SDL_CONTROLLERDEVICEADDED) {
        gamepad_open_first();
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
    return vb_host_controller_pad(s_pad, vb_host_config);
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
    const int volume = s_runtime_audio_volume.load(std::memory_order_relaxed);
    if (volume < 100) {
        int16_t* samples = reinterpret_cast<int16_t*>(stream);
        const size_t count = (size_t)len / sizeof(int16_t);
        for (size_t i = 0; i < count; ++i)
            samples[i] = (int16_t)((int)samples[i] * volume / 100);
    }
}
#endif

int main(int argc, char** argv) {
    int port = VB_DEFAULT_DEBUG_PORT;
    const char* rom_path = nullptr;
    bool headless = false;
    bool stereo = false;
    bool start_paused = false;

    for (int i = 1; i < argc; ++i) {
        const int host_arg = vb_host_argument(i, argc, argv);
        if (host_arg < 0) { std::puts(vb_host_error().c_str()); return 2; }
        if (host_arg > 0) continue;
        std::string a = argv[i];
        if ((a == "--help" || a == "-h")) {
            print_help(argv[0]);
            return 0;
        } else if (a == "--port" && i + 1 < argc) {
            port = std::atoi(argv[++i]);
        } else if (a == "--rom" && i + 1 < argc) {
            rom_path = argv[++i];
        } else if (a == "--execution" && i + 1 < argc) {
            std::string mode=argv[++i];
            if(mode=="hybrid") vb_execution.mode=VB_EXEC_HYBRID;
            else if(mode=="native") vb_execution.mode=VB_EXEC_NATIVE;
            else if(mode=="interpreter") vb_execution.mode=VB_EXEC_INTERPRETER;
            else { std::puts("Invalid --execution mode"); return 2; }
        } else if (a == "--paused") {
            start_paused = true;
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

#if !defined(VBRECOMP_DEBUG_TOOLS)
    if (start_paused) { std::fputs("--paused requires a debug-tools build.\n", stderr); return 2; }
#endif
    const int host_result = vb_host_prepare(argv[0], rom_path, headless);
    if (host_result < 0) { std::puts(vb_host_error().c_str()); return 2; }
    if (host_result == 0) return 0;
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
        if (!vb_memory_activate_rom_patches()) {
            std::fputs("vb-runtime: ROM data plan failed byte guards or bounds.\n", stderr);
            vb_memory_shutdown();
            return 7;
        }
        cpu.read8 = vb_read8;
        cpu.read16 = vb_read16;
        cpu.read32 = vb_read32;
        cpu.write8 = vb_write8;
        cpu.write16 = vb_write16;
        cpu.write32 = vb_write32;
        vb_cpu_reset(&cpu);
        vb_memory_set_cpu(&cpu);
        if (!vb_host_load_sram()) { std::puts(vb_host_error().c_str()); vb_memory_shutdown(); return 6; }
        // Architectural r31 remains zero at reset.
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

    vb_debug_server_set_paused(start_paused);
    using namespace std::chrono_literals;

#if VB_RUNTIME_HAVE_SDL
    SDL_Window*       win = nullptr;
    SDL_Renderer*     ren = nullptr;
    SDL_Texture*      tex = nullptr;
    SDL_AudioDeviceID aud = 0;
    Uint64            sdl_freq = 0;
    Uint64            sdl_period = 0;
    Uint64            sdl_deadline = 0;
    int               tex_w = VB_RT_EYE_W;
    const int         tex_h = stereo ? VB_RT_EYE_H * 2 : VB_RT_EYE_H;
    std::vector<uint32_t> tex_storage(VB_VIEWPORT_MAX_WIDTH * VB_RT_EYE_H * 2);
    uint32_t*         tex_pixels = tex_storage.data();
    VbHostCapture host_capture{tex_pixels, tex_w, tex_h};
    if (!headless) vb_debug_server_set_host_capture(vb_capture_host, &host_capture);
#if defined(RECOMP_LAUNCHER)
    VbRuntimeUiContext runtime_ui_context = {};
    std::vector<RecompLauncherCModFeature> ui_features;
    std::vector<std::string> ui_feature_keys;
    std::vector<RecompRuntimeUiItem> ui_mod_items;
#endif

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
            tex_w * vb_host_config.scale, tex_h * vb_host_config.scale,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!win) {
            std::fprintf(stderr, "vb-runtime: SDL_CreateWindow failed: %s\n",
                         SDL_GetError());
            headless = true;
        }
    }
    if (!headless && vb_host_config.fullscreen)
        SDL_SetWindowFullscreen(win, vb_host_config.fullscreen == 2 ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP);
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
                    tex_w * vb_host_config.scale, tex_h * vb_host_config.scale,
                    VB_RT_FRAME_HZ,
                    stereo ? "L eye top / R eye bottom"
                           : "single eye (--stereo for both)");
        std::fflush(stdout);

        /* Audio device. Failure is non-fatal — the window keeps
         * running with the cart's writes accumulating into the VSU
         * ring but no sound out. */
        s_runtime_audio_volume = vb_host_config.volume;
#if SDL_VERSION_ATLEAST(2, 0, 12)
        SDL_SetTextureScaleMode(tex, vb_host_config.filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#endif
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
            SDL_PauseAudioDevice(aud, vb_host_config.audio ? 0 : 1);
            std::printf("vb-runtime: SDL audio at %d Hz, %d-frame "
                        "buffer\n", have.freq, have.samples);
            std::fflush(stdout);
        }
#if defined(RECOMP_LAUNCHER)
        runtime_ui_context.window = win;
        runtime_ui_context.texture = tex;
        runtime_ui_context.audio = aud;
        runtime_ui_context.window_scale = vb_host_config.scale;
        runtime_ui_context.linear_filter = vb_host_config.filter;
        runtime_ui_context.audio_enabled = aud != 0 && vb_host_config.audio;
        runtime_ui_context.volume = vb_host_config.volume;
        s_runtime_audio_volume = vb_host_config.volume;
        RecompRuntimeUiStandardConfig runtime_ui_config = {};
        runtime_ui_config.menu.title = "Virtual Boy Recompiled";
        runtime_ui_config.menu.subtitle = "Runtime settings";
        runtime_ui_config.menu.theme = "vb";
        runtime_ui_config.menu.accept_label = "A / Enter";
        runtime_ui_config.menu.back_label = "B / Esc";
        runtime_ui_config.menu.callbacks = {
            &runtime_ui_context, vb_runtime_ui_get, vb_runtime_ui_set,
            vb_runtime_ui_action, nullptr, vb_host_save, nullptr
        };
        runtime_ui_config.features = RECOMP_RUNTIME_UI_STANDARD_FULLSCREEN |
            RECOMP_RUNTIME_UI_STANDARD_WINDOW_SCALE |
            RECOMP_RUNTIME_UI_STANDARD_LINEAR_FILTER |
            RECOMP_RUNTIME_UI_STANDARD_AUDIO |
            RECOMP_RUNTIME_UI_STANDARD_VOLUME |
            RECOMP_RUNTIME_UI_STANDARD_RESUME;
        runtime_ui_config.window_scale_max = 6;
        if (const auto* provider = vb_mod_runtime_launcher_provider_c()) {
            const int count = provider->feature_count(provider->ctx);
            ui_features.resize(count);
            ui_feature_keys.resize(count);
            ui_mod_items.resize(count);
            for (int i = 0; i < count; ++i) {
                provider->feature_get(provider->ctx, i, &ui_features[i]);
                const auto& f = ui_features[i];
                ui_feature_keys[i] = std::string("mod:") + f.package_id + ":" + f.id;
                ui_mod_items[i] = {ui_feature_keys[i].c_str(), "Mods", f.name, f.description,
                    RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr};
            }
            runtime_ui_config.extra_items = ui_mod_items.data();
            runtime_ui_config.extra_item_count = ui_mod_items.size();
        }
        s_runtime_ui = recomp_runtime_ui_create_standard(&runtime_ui_config);
#endif
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
    constexpr uint64_t VB_CYCLES_PER_FRAME = 259u * 384u * 4u;
    uint64_t last_present_cycles = 0;
    /* Cycle-driven frame counter (Axis-6). Independent of the SDL present
     * path so cpu.frame advances in --headless too; cpu.frame was previously
     * stuck at 0 (the `frame` TCP command and the frame ring both read it). */
    uint64_t last_frame_cycles = 0;
    bool sdl_quit = false;

    // P4-A main loop: alternate between recompiled-code dispatch
    // and TCP polling, and tick device emulation (timer, VIP) by the
    // V810 cycle delta consumed each pass. Between passes, check the
    // IRQ controller and route to an interrupt vector when one is
    // pending and accepted.
    //
    // Cycle accounting (Axis-2 cycle model): the recompiled code charges
    // each instruction's real V810 base cost into cpu.cycles as it runs
    // (recompiler/v810/cycles.py). The per-pass device-tick delta is the
    // cpu.cycles advance, captured around vb_dispatch below. cpu.step_budget
    // is a SEPARATE concern — the per-basic-block yield budget, decremented
    // once per BB leader so a tight intra-function loop still yields to the
    // TCP/SDL poll; it no longer feeds the cycle estimate.
    constexpr uint64_t STEP_BUDGET     = 250000;
    bool dispatched_once = false;
    uint32_t dispatch_pc = cpu.pc;
    uint64_t present_count = 0;
    /* Always-on hang watchdog (separate thread). Started here, from the main
     * thread, so it can capture this thread's stack on a freeze. */
    vb_watchdog_start();
    while (vb_debug_server_poll() == 0 && !sdl_quit) {
        vb_watchdog_beat(VB_WD_POLL, dispatch_pc, cpu.cycles, present_count);

        /* Axis-6 frame ring: advance the VIP-frame counter from elapsed
         * cycles (covers both the active-dispatch and HALT-idle paths, which
         * both pass through here) and record a per-frame {pc,gpr,psw}
         * fingerprint into the always-on ring. The ring is queried, never
         * armed (debug `frame` command reports the seq). Catch-up loop in
         * case a single pass spanned more than one frame period. */
        while (cpu.cycles - last_frame_cycles >= VB_CYCLES_PER_FRAME) {
            last_frame_cycles += VB_CYCLES_PER_FRAME;
            vb_devices_end_frame(cpu.cycles);
            cpu.frame++;
            vb_ring_frame_record(&cpu);
        }
#if VB_RUNTIME_HAVE_SDL
        if (!headless) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { sdl_quit = true; break; }
#if defined(RECOMP_LAUNCHER)
                if (vb_runtime_ui_event(ev)) continue;
#endif
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
                        if (SDL_SetWindowFullscreen(win,
                            is_fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP) == 0) {
                            vb_host_config.fullscreen = is_fs ? 0 : 1;
                            vb_host_save();
                        }
                    }
                }
            }
            if (sdl_quit) break;
        }
#endif
        /* The configured keyboard/controller drives a windowed game unless
         * a TCP input override is active. clear_input restores physical input. */
#if VB_RUNTIME_HAVE_SDL
        if (!headless && !vb_debug_server_input_override()) {
            uint16_t controller_pad = pad_from_gamecontroller(nullptr);
#if defined(RECOMP_LAUNCHER)
            if (s_runtime_ui && recomp_runtime_ui_is_open(s_runtime_ui))
                vb_input_set_pad(0);
            else
#endif
                vb_input_set_pad((uint16_t)(pad_from_keyboard() | controller_pad));
        }
#endif

        if (!rom_path) {
            std::this_thread::sleep_for(2ms);
            continue;
        }

        bool simulation_paused = vb_debug_server_is_paused() != 0;
#if defined(RECOMP_LAUNCHER) && VB_RUNTIME_HAVE_SDL
        simulation_paused = simulation_paused || (s_runtime_ui && recomp_runtime_ui_is_open(s_runtime_ui));
#endif
        if (!simulation_paused) {
        // Deliver any pending IRQ before re-entering dispatch.
        // vb_irq_check_and_deliver retargets cpu.pc to the vector
        // when accepted and clears cpu.halted; the next dispatch
        // call will start executing the ISR.
        if (vb_irq_check_and_deliver(&cpu)) {
            dispatch_pc = cpu.pc;
        }

        if (cpu.halted) {
            // Event-driven idle (Axis-3 IRQ-take precision). Advance device
            // time to the next device-state boundary (VIP column/drawing,
            // timer divider) instead of a fixed 20000-cycle chunk, breaking
            // as soon as an ACCEPTABLE IRQ is pending (same acceptance rule
            // as vb_irq_check_and_deliver, which the top of the loop then
            // applies). IRQ-take latency drops from ~20000 cyc to ~one
            // column (259 cyc), and we sleep once per wake instead of ~20x
            // per frame. The frame-sized cap bounds a pass when nothing is
            // deliverable (e.g. all sources masked).
            uint64_t idle_consumed = 0;
            while (cpu.cycles < last_frame_cycles + VB_CYCLES_PER_FRAME) {
                int32_t step = vb_devices_cycles_to_next_event(cpu.cycles);
                if (step < 1) step = 1;
                uint64_t frame_remaining=last_frame_cycles + VB_CYCLES_PER_FRAME-cpu.cycles;
                if ((uint64_t)step > frame_remaining) step=(int32_t)frame_remaining;
                cpu.cycles += (uint64_t)step;
                vb_devices_sync(cpu.cycles);
                idle_consumed += (uint64_t)step;
                const int lvl = vb_irq_highest_pending_level();
                if (lvl >= 0 && !cpu.psw_np && !cpu.psw_ep && !cpu.psw_id
                    && lvl >= (int)cpu.psw_int_level)
                    break;   // acceptable IRQ pending — loop top will deliver
            }
            std::this_thread::sleep_for(1ms);
            continue;
        }

        // Axis-3 mid-block IRQ take: bound this dispatch pass to the next
        // device-event cycle (VIP column / timer divider — the same
        // boundaries the HALT idle loop steps to). The recompiled per-BB
        // check yields once cpu.cycles reaches the deadline, so an IRQ that
        // becomes pending mid-pass is delivered (top-of-loop
        // vb_irq_check_and_deliver) within one basic block of the true
        // event instead of up to a 250000-block pass later. This removes the
        // interrupt-latency a timer-re-arming music ISR would accumulate
        // into tempo drift. STEP_BUDGET remains the backstop for the case
        // where no device event is near (both sources disabled).
        {
            int32_t ev_next = vb_devices_cycles_to_next_event(cpu.cycles);
            if (ev_next < 1) ev_next = 1;
            cpu.cycle_deadline = cpu.cycles + (uint64_t)ev_next;
            if (cpu.cycle_deadline > last_frame_cycles + VB_CYCLES_PER_FRAME)
                cpu.cycle_deadline = last_frame_cycles + VB_CYCLES_PER_FRAME;
        }
        cpu.step_budget = STEP_BUDGET;
        cpu.yielded = 0;
        vb_watchdog_beat(VB_WD_DISPATCH, dispatch_pc, cpu.cycles, present_count);
        const uint64_t cyc_before = cpu.cycles;
        vb_dispatch(&cpu, dispatch_pc);

        // Axis-2 cycle model: the recompiled code now accumulates each
        // instruction's real V810 base cost into cpu.cycles as it runs
        // (recompiler/v810/cycles.py), so the device-tick delta is just
        // how far cpu.cycles advanced this pass — no more bbs_run*3
        // estimate. step_budget is unchanged: it remains the per-basic-
        // block yield budget, fully decoupled from cycle accounting.
        const uint64_t cyc_delta = cpu.cycles - cyc_before;
        vb_devices_sync(cpu.cycles);

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
            vb_stub_abort("dispatcher returned without yielding or halting", cpu.pc, 0);
        }

        } else {
            std::this_thread::sleep_for(2ms);
        }
#if VB_RUNTIME_HAVE_SDL
        if (!headless && (simulation_paused || (cpu.cycles - last_present_cycles) >= VB_CYCLES_PER_FRAME)) {
            last_present_cycles = cpu.cycles;
            vb_watchdog_beat(VB_WD_PRESENT, dispatch_pc, cpu.cycles, ++present_count);

            int win_w = 0, win_h = 0;
            SDL_GetRendererOutputSize(ren, &win_w, &win_h);
            vb_viewport_window(win_w, stereo ? win_h / 2 : win_h);
            const int view_width = vb_renderer_present_width();
            if (view_width != tex_w) {
                SDL_Texture* resized = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING, view_width, tex_h);
                if (!resized) vb_stub_abort("cannot resize presentation texture", cpu.pc, view_width);
                SDL_DestroyTexture(tex); tex = resized; tex_w = view_width;
                host_capture.width = tex_w;
#if SDL_VERSION_ATLEAST(2, 0, 12)
                SDL_SetTextureScaleMode(tex, vb_host_config.filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
#endif
#if defined(RECOMP_LAUNCHER)
                runtime_ui_context.texture = tex;
#endif
            }

            /* Opt-in full-screen recolor uses the recolored present path;
             * otherwise the faithful render. */
            const bool recolor = vb_recolor_active();
            if (vb_renderer_active()) {
                if (!vb_renderer_present_viewport(0, tex_w, tex_pixels))
                    vb_stub_abort("viewport frame unavailable", cpu.pc, tex_w);
                if (stereo && !vb_renderer_present_viewport(1, tex_w, tex_pixels + tex_w * VB_RT_EYE_H))
                    vb_stub_abort("right viewport frame unavailable", cpu.pc, tex_w);
            } else if (recolor) {
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
                static uint32_t center[VB_RT_EYE_W * VB_RT_EYE_H];
                for (int eye = 0; eye < (stereo ? 2 : 1); ++eye) {
                    uint32_t* image = tex_pixels + eye * tex_w * VB_RT_EYE_H;
                    int margin = (tex_w - VB_RT_EYE_W) / 2;
                    for (int y = 0; y < VB_RT_EYE_H; ++y)
                        std::memcpy(center + y * VB_RT_EYE_W, image + y * tex_w + margin, VB_RT_EYE_W * sizeof(uint32_t));
                    vb_overlay_composite(center, VB_RT_EYE_W, VB_RT_EYE_H, eye, slot);
                    for (int y = 0; y < VB_RT_EYE_H; ++y)
                        std::memcpy(image + y * tex_w + margin, center + y * VB_RT_EYE_W, VB_RT_EYE_W * sizeof(uint32_t));
                }
            }

#if defined(RECOMP_LAUNCHER)
            recomp_runtime_ui_render_argb8888(s_runtime_ui, tex_pixels,
                                               tex_w, tex_h,
                                               tex_w * (int)sizeof(uint32_t));
#endif

            SDL_UpdateTexture(tex, nullptr, tex_pixels,
                              tex_w * (int)sizeof(uint32_t));

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
                // Turbo must not accumulate future deadlines: releasing Tab
                // would otherwise sleep away every frame advanced at turbo speed.
                sdl_deadline = SDL_GetPerformanceCounter() + sdl_period;
            }
        }
#endif
    }

    vb_watchdog_stop();
#if defined(RECOMP_LAUNCHER) && VB_RUNTIME_HAVE_SDL
    recomp_runtime_ui_destroy(s_runtime_ui);
    s_runtime_ui = nullptr;
#endif

#if VB_RUNTIME_HAVE_SDL
    if (s_pad) { SDL_GameControllerClose(s_pad); s_pad = nullptr; }
    if (aud) { SDL_PauseAudioDevice(aud, 1); SDL_CloseAudioDevice(aud); }
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    if (!headless) SDL_Quit();
#endif

    bool save_ok = !rom_path || vb_host_save_sram();
    if (!save_ok) std::puts(vb_host_error().c_str());
    vb_mod_runtime_deactivate();
    vb_debug_server_stop();
    vb_memory_shutdown();
    vb_ring_frame_shutdown();
    vb_wtrace_shutdown();
    vb_fntrace_shutdown();
    return save_ok ? 0 : 6;
}
