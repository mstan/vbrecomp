/* main.cpp — vb-runtime entry point.
 *
 * Drives the recompiled cart in a step-budget dispatch loop, ticks
 * device emulation (timer + VIP) by the cycle delta consumed each
 * pass, services the TCP debug server, and (when SDL2 is available)
 * presents the latest VIP framebuffer at 50.27 Hz with keyboard
 * input feeding vb_input_set_pad.
 */
#include <chrono>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <ctime>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#endif

static double process_cpu_seconds() {
#ifdef _WIN32
    FILETIME created, exited, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) return -1;
    ULARGE_INTEGER k, u;
    k.LowPart=kernel.dwLowDateTime; k.HighPart=kernel.dwHighDateTime;
    u.LowPart=user.dwLowDateTime; u.HighPart=user.dwHighDateTime;
    return (k.QuadPart + u.QuadPart) * 1e-7;
#else
    return (double)std::clock() / CLOCKS_PER_SEC;
#endif
}

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
#include "machine.h"
#include "savestate.h"
#include "game_host.h"
#include "netplay.h"
#include "state_ui.h"
static int can_pause_session(){return !vb_netplay_enabled() && vb_game_host_can_pause();}
#include "vb_sha256.h"
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
#  include "host_controllers.h"
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
#ifndef VB_GAME_TITLE
#define VB_GAME_TITLE VB_DEFAULT_WINDOW_TITLE
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
        "  --headless       Do not open an SDL window.\n"
        "  --frames N       Exit cleanly after N guest frames (1..1000000).\n"
        "  --stereo         Show both eyes stacked vertically (L top / R bottom)\n"
        "                   instead of the single-eye default.\n"
        "  --benchmark N    Run N uncapped headless frames and exit.\n"
        "  --measure-runtime N  Run N uncapped frames with SDL render/present/audio.\n"
        "  --benchmark-route PATH  Decimal frame-count and pad-mask per line.\n"
        "  --state-file PATH  F5 quick save / F6 quick load; F7 slot browser.\n"
        "  --rewind-frames N  Keep 2..512 local snapshots; F8 opens rewind.\n"
        "  --rewind-interval N  Frames per snapshot (1..60; CLI depth defaults 1).\n"
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
static std::atomic<bool> s_state_modal{false};
static bool s_lobby_requested;
static unsigned s_history_interval=1;
struct VbHostCapture { const uint32_t* pixels; int width, height; };
static int vb_capture_host(const char* path, void* context) {
    const auto* frame = static_cast<VbHostCapture*>(context);
    return vb_write_png_32bpp(path, frame->width, frame->height, frame->pixels);
}
static int vb_inject_host_input(int device,int value,int pressed,int repeat,void* context) {
    SDL_Event event{};
    if(!device) {
        event.type=pressed?SDL_KEYDOWN:SDL_KEYUP;
        event.key.windowID=SDL_GetWindowID(static_cast<SDL_Window*>(context));
        event.key.keysym.scancode=(SDL_Scancode)value;
        event.key.keysym.sym=SDL_GetKeyFromScancode((SDL_Scancode)value);
        event.key.repeat=(Uint8)repeat;
    } else {
        event.type=pressed?SDL_CONTROLLERBUTTONDOWN:SDL_CONTROLLERBUTTONUP;
        event.cbutton.button=(Uint8)value;
    }
    return SDL_PushEvent(&event)>0;
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
    else if (std::strcmp(item->key, "history.enabled") == 0) *out = vb_host_config.rewind;
    else if (std::strcmp(item->key, "history.depth") == 0) *out = vb_host_config.rewind_depth;
    else if (std::strcmp(item->key, "history.interval") == 0) *out = (int)s_history_interval;
    else return 0;
    return 1;
}

static int vb_runtime_ui_set(void* opaque, const RecompRuntimeUiItem* item, int value) {
    auto* c = static_cast<VbRuntimeUiContext*>(opaque);
    if (!c || !item) return 0;
    std::string package, feature;
    if (vb_ui_mod_identity(item, package, feature)) {
        if(vb_netplay_enabled())return 0; // the negotiated feature set is fixed for this match
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
    } else if (std::strcmp(item->key, "history.enabled") == 0 || std::strcmp(item->key, "history.depth") == 0) {
        if(vb_netplay_enabled())return 0;
        const bool enabled=std::strcmp(item->key,"history.enabled")==0?value!=0:vb_host_config.rewind!=0;
        const int depth=std::strcmp(item->key,"history.depth")==0?value:vb_host_config.rewind_depth;
        if(enabled && !vb_rewind_enable((unsigned)depth))return 0;
        if(!enabled)vb_rewind_shutdown();
        vb_state_ui_sync();vb_host_config.rewind=enabled;vb_host_config.rewind_depth=depth;
    } else if (std::strcmp(item->key, "history.interval") == 0) {
        if(vb_netplay_enabled())return 0;
        s_history_interval=(unsigned)std::max(1,std::min(value,60));
        vb_host_config.rewind_interval=(int)s_history_interval;
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
    if(item && !std::strcmp(item->key,"session.lobby") && vb_netplay_from_lobby()) {
        s_lobby_requested=true;return 1;
    }
    if(item && (!std::strcmp(item->key,"history.browser") || !std::strcmp(item->key,"history.rewind"))) {
        recomp_runtime_ui_close(s_runtime_ui);
        return vb_state_ui_action(!std::strcmp(item->key,"history.browser")?VB_STATE_UI_SLOTS:VB_STATE_UI_REWIND);
    }
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

static bool vb_state_ui_event(const SDL_Event& ev) {
    if(ev.type==SDL_KEYDOWN && (ev.key.keysym.sym==SDLK_F7 || ev.key.keysym.sym==SDLK_F8)) {
        if(!ev.key.repeat) {
#if defined(RECOMP_LAUNCHER)
            if(s_runtime_ui)recomp_runtime_ui_close(s_runtime_ui);
#endif
            vb_state_ui_action(ev.key.keysym.sym==SDLK_F7?VB_STATE_UI_SLOTS:VB_STATE_UI_REWIND);
        }
        return true;
    }
    if(!vb_state_ui_mode())return false;
    if(ev.type==SDL_KEYDOWN) {
        int key=ev.key.keysym.sym,action=-1;
        int jump=key>=SDLK_1&&key<=SDLK_9?key-SDLK_1:key==SDLK_0?9:key==SDLK_MINUS?10:key==SDLK_EQUALS?11:-1;
        if(jump>=0){vb_state_ui_select_slot(jump);return true;}
        switch(key) {
            case SDLK_ESCAPE: case SDLK_BACKSPACE: action=VB_STATE_UI_BACK;break;
            case SDLK_LEFT: action=VB_STATE_UI_LEFT;break;
            case SDLK_RIGHT: action=VB_STATE_UI_RIGHT;break;
            case SDLK_UP: action=VB_STATE_UI_UP;break;
            case SDLK_DOWN: action=VB_STATE_UI_DOWN;break;
            case SDLK_s: action=VB_STATE_UI_SAVE;break;
            case SDLK_x: case SDLK_l: case SDLK_RETURN: case SDLK_SPACE: action=VB_STATE_UI_LOAD;break;
        }
        if(action>=0 && (!ev.key.repeat || (action>=VB_STATE_UI_LEFT&&action<=VB_STATE_UI_DOWN)))vb_state_ui_action(action);
        return true;
    }
    if(ev.type==SDL_CONTROLLERBUTTONDOWN) {
        int action=-1;
        switch(ev.cbutton.button) {
            case SDL_CONTROLLER_BUTTON_A: action=VB_STATE_UI_LOAD;break;
            case SDL_CONTROLLER_BUTTON_X: action=VB_STATE_UI_SAVE;break;
            case SDL_CONTROLLER_BUTTON_B: action=VB_STATE_UI_BACK;break;
        }
        if(action>=0)vb_state_ui_action(action);
        return true;
    }
    return ev.type==SDL_KEYUP || ev.type==SDL_CONTROLLERBUTTONUP;
}

static uint16_t pad_from_keyboard(unsigned seat=0) {
    /* Hardware pad masks use one bit per pressed button. */
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    uint16_t pad = 0;
    if (!keys) return pad;
    const auto& player=vb_host_player(seat);
    if (player.player_source != 1) return 0;
    for (int i = 0; i < 14; ++i) {
        const int key = player.keys[i];
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
static VbHostControllers s_controllers;
static auto& s_pads=s_controllers.pads;
static SDL_GameController*& s_pad=s_pads[0];

/* Open the first attached controller, if any and none is open yet. */
static void gamepad_open_first(void) {
    s_controllers.refresh();
}

/* React to SDL_CONTROLLERDEVICEADDED / REMOVED so hotplug works. */
static void gamepad_handle_device_event(const SDL_Event& ev) {
    if (ev.type == SDL_CONTROLLERDEVICEADDED) {
        gamepad_open_first();
    } else if (ev.type == SDL_CONTROLLERDEVICEREMOVED) {
        s_controllers.removed(ev.cdevice.which);
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
    if(s_state_modal.load(std::memory_order_relaxed) || vb_netplay_in_resim()){std::memset(stream,0,(size_t)len);return;}
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
#ifdef _WIN32
    // Windows' narrow CRT argv can lose Unicode paths before filesystem code
    // sees them. Match the debugger's UTF-8 path contract at the entry point.
    int wide_count=0;
    auto wide_args=CommandLineToArgvW(GetCommandLineW(),&wide_count);
    std::vector<std::string> utf8_args;
    std::vector<char*> utf8_argv;
    if(!wide_args)return 2;
    for(int i=0;i<wide_count;++i) {
        int bytes=WideCharToMultiByte(CP_UTF8,0,wide_args[i],-1,nullptr,0,nullptr,nullptr);
        if(bytes<=0){LocalFree(wide_args);return 2;}
        std::string value((size_t)bytes,'\0');
        WideCharToMultiByte(CP_UTF8,0,wide_args[i],-1,value.data(),bytes,nullptr,nullptr);
        value.pop_back();utf8_args.push_back(std::move(value));
    }
    LocalFree(wide_args);
    for(auto& value:utf8_args)utf8_argv.push_back(value.data());
    utf8_argv.push_back(nullptr);argc=wide_count;argv=utf8_argv.data();
#endif
    int port = VB_DEFAULT_DEBUG_PORT;
    const char* rom_path = nullptr;
    bool headless = false;
    bool explicit_headless = false;
    bool stereo = false;
    bool start_paused = false;
    uint32_t frame_limit = 0;
    std::string state_file = "quick.state";
    unsigned rewind_depth = 0;
    unsigned rewind_interval=1;
    bool rewind_explicit=false,interval_explicit=false;
    uint32_t benchmark_frames = 0;
    bool measure_runtime = false;
    int measure_window_width = 0, measure_window_height = 0;
    const char* benchmark_route_path = nullptr;
    struct BenchmarkSegment { uint32_t end; uint16_t pad; };
    std::vector<BenchmarkSegment> benchmark_route;


    for (int i = 1; i < argc; ++i) {
        const int net_arg=vb_netplay_option(argc,argv,&i);
        if(net_arg<0){std::fprintf(stderr,"vb-runtime: %s\n",vb_netplay_error());return 2;}
        if(net_arg>0)continue;
        const int host_arg = vb_host_argument(i, argc, argv);
        if (host_arg < 0) { std::puts(vb_host_error().c_str()); return 2; }
        if (host_arg > 0) continue;
        const int game_arg=vb_game_host_option(argc,argv,&i);
        if(game_arg<0){std::fprintf(stderr,"vb-runtime: %s\n",vb_game_host_error());return 2;}
        if(game_arg>0)continue;
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
        } else if (a == "--frames" && i + 1 < argc) {
            std::string count=argv[++i];
            if(count.empty() || count.find_first_not_of("0123456789")!=std::string::npos) {
                std::puts("--frames requires a positive decimal count (maximum 1000000)");return 2;
            }
            unsigned long long value=std::strtoull(count.c_str(),nullptr,10);
            if(!value || value>1000000) { std::puts("Invalid --frames count");return 2; }
            frame_limit=(uint32_t)value;
        } else if ((a == "--benchmark" || a == "--measure-runtime") && i + 1 < argc) {
            if (benchmark_frames) { std::fputs("Only one finite measurement option is allowed.\n", stderr); return 2; }
            measure_runtime = a == "--measure-runtime";
            char* end = nullptr;
            const char* value = argv[++i];
            unsigned long n = std::strtoul(value, &end, 10);
            if (!*value || *end || *value == '-' || !n || n > 1000000) {
                std::fputs("--benchmark requires 1..1000000 frames.\n", stderr); return 2;
            }
            benchmark_frames = (uint32_t)n;
            headless = !measure_runtime;
        } else if (a == "--benchmark-route" && i + 1 < argc) {
            benchmark_route_path = argv[++i];
        } else if (a == "--measure-window" && i + 1 < argc) {
            char extra;
            if (std::sscanf(argv[++i], "%dx%d%c", &measure_window_width, &measure_window_height, &extra) != 2 ||
                measure_window_width < 128 || measure_window_width > 8192 ||
                measure_window_height < 128 || measure_window_height > 8192) {
                std::fputs("--measure-window requires WIDTHxHEIGHT in 128..8192\n", stderr); return 2;
            }
        } else if (a == "--state-file" && i + 1 < argc) {
            state_file = argv[++i];
        } else if (a == "--rewind-frames" && i + 1 < argc) {
            char* end=nullptr;const char* value=argv[++i];
            unsigned long n=std::strtoul(value,&end,10);
            if(!*value || *end || *value=='-' || n<2 || n>512) {
                std::puts("--rewind-frames requires 2..512");return 2;
            }
            rewind_depth=(unsigned)n;rewind_explicit=true;
        } else if (a == "--rewind-interval" && i + 1 < argc) {
            char* end=nullptr;const char* value=argv[++i];unsigned long n=std::strtoul(value,&end,10);
            if(!*value || *end || *value=='-' || n<1 || n>60){std::puts("--rewind-interval requires 1..60");return 2;}
            rewind_interval=(unsigned)n;interval_explicit=true;
        } else if (a == "--paused") {
            start_paused = true;
        } else if (a == "--headless") {
            headless = true;
            explicit_headless = true;
        } else if (a == "--stereo" || a == "--dual-eye") {
            stereo = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s (try --help)\n", argv[i]);
            return 2;
        }
    }

    if (benchmark_frames && frame_limit) {
        std::fputs("--frames cannot be combined with a finite measurement.\n", stderr); return 2;
    }
    if (benchmark_frames && (start_paused || !rom_path)) {
        std::fputs("--benchmark requires --rom and cannot use --paused.\n", stderr); return 2;
    }
    if (measure_runtime && explicit_headless) {
        std::fputs("--measure-runtime cannot use --headless.\n", stderr); return 2;
    }
    if (measure_window_width && !measure_runtime) {
        std::fputs("--measure-window requires --measure-runtime\n", stderr); return 2;
    }
    if (benchmark_route_path) {
        if (!benchmark_frames) { std::fputs("--benchmark-route requires --benchmark.\n", stderr); return 2; }
        FILE* route = std::fopen(benchmark_route_path, "r");
        if (!route) { std::perror("benchmark route"); return 2; }
        char line[128]; unsigned total = 0; bool valid = true;
        while (std::fgets(line, sizeof(line), route)) {
            unsigned long long frames = 0, pad = 0; char extra;
            if (std::sscanf(line, "%llu %llu %c", &frames, &pad, &extra) != 2 ||
                !frames || pad > 65535 || frames > benchmark_frames - total) { valid = false; break; }
            total += (unsigned)frames; benchmark_route.push_back({total, (uint16_t)pad});
        }
        if (std::ferror(route)) valid = false;
        std::fclose(route);
        if (!valid || total != benchmark_frames) {
            std::fputs("Benchmark route must contain positive counts and 16-bit pad masks totaling N.\n", stderr); return 2;
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
    const int host_result = vb_host_prepare(argv[0], rom_path, headless || benchmark_frames != 0);
    if (host_result < 0) { std::puts(vb_host_error().c_str()); return 2; }
    if (host_result == 0) return 0;
    if(measure_runtime && (headless || !vb_host_config.audio)) {
        std::puts("Runtime measurement requires working SDL presentation and audio");return 3;
    }
    bool first_session=true;
    const unsigned initial_rewind_depth=rewind_depth;
    for(;;) {
    rewind_depth=initial_rewind_depth;
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

    using namespace std::chrono_literals;

#if VB_RUNTIME_HAVE_SDL
    SDL_Window*       win = nullptr;
    SDL_Renderer*     ren = nullptr;
    SDL_Texture*      tex = nullptr;
    SDL_Texture*      state_tex = nullptr;
    uint16_t menu_blocked_pad=0,previous_gesture=0;
    int held_menu_direction=0;
    Uint32 next_menu_repeat=0;
    SDL_AudioDeviceID aud = 0;
    Uint64            sdl_freq = 0;
    Uint64            sdl_period = 0;
    Uint64            sdl_deadline = 0;
    int               tex_w = VB_RT_EYE_W;
    int               tex_h = stereo ? VB_RT_EYE_H * 2 : VB_RT_EYE_H;
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
            explicit_headless = true;
        } else {
            gamepad_open_first();
        }
    }
    if (!headless) {
        win = SDL_CreateWindow(
            VB_GAME_TITLE,
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            measure_window_width ? measure_window_width : tex_w * vb_host_config.scale,
            measure_window_height ? measure_window_height : tex_h * vb_host_config.scale,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!win) {
            std::fprintf(stderr, "vb-runtime: SDL_CreateWindow failed: %s\n",
                         SDL_GetError());
            headless = true;
            explicit_headless = true;
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
                                 SDL_RENDERER_ACCELERATED | (measure_runtime ? 0 : SDL_RENDERER_PRESENTVSYNC));
        if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        if (!ren) ren = SDL_CreateRenderer(win, -1, 0);
        if (!ren) {
            std::fprintf(stderr, "vb-runtime: SDL_CreateRenderer failed: %s\n",
                         SDL_GetError());
            headless = true;
            explicit_headless = true;
        }
    }
    if (!headless) {
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING,
                                tex_w, tex_h);
        state_tex=SDL_CreateTexture(ren,SDL_PIXELFORMAT_ARGB8888,SDL_TEXTUREACCESS_STREAMING,768,448);
        if (!tex || !state_tex) {
            std::fprintf(stderr, "vb-runtime: SDL_CreateTexture failed: %s\n",
                         SDL_GetError());
            headless = true;
            explicit_headless = true;
        }
    }
    if (!headless) {
        sdl_freq = SDL_GetPerformanceFrequency();
        vb_debug_server_set_host_input(vb_inject_host_input,win);
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
            if(measure_runtime) return 3;
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
            ui_mod_items.reserve(count+5);
            for (int i = 0; i < count; ++i) {
                provider->feature_get(provider->ctx, i, &ui_features[i]);
                const auto& f = ui_features[i];
                ui_feature_keys[i] = std::string("mod:") + f.package_id + ":" + f.id;
                ui_mod_items.push_back({ui_feature_keys[i].c_str(), "Mods", f.name, f.description,
                    RECOMP_RUNTIME_UI_BOOL, 0, 1, 1, nullptr, 0, nullptr});
            }
        }
        if(!vb_netplay_enabled()) {
            ui_mod_items.push_back({"history.browser","System","Save states (F7)","Browse twelve thumbnail slots.",RECOMP_RUNTIME_UI_ACTION,0,0,0,nullptr,0,nullptr});
            ui_mod_items.push_back({"history.rewind","System","Rewind (F8)","Browse local history and select a point to resume.",RECOMP_RUNTIME_UI_ACTION,0,0,0,nullptr,0,nullptr});
            ui_mod_items.push_back({"history.enabled","Rewind","Enable rewind","Keep local execution snapshots while playing.",RECOMP_RUNTIME_UI_BOOL,0,1,1,nullptr,0,nullptr});
            ui_mod_items.push_back({"history.depth","Rewind","Snapshots","History slots, subject to a 512 MiB limit.",RECOMP_RUNTIME_UI_INT,8,64,8,nullptr,0,nullptr});
            ui_mod_items.push_back({"history.interval","Rewind","Frames per snapshot","Larger intervals retain more seconds of history.",RECOMP_RUNTIME_UI_INT,1,60,1,nullptr,0,nullptr});
        } else if(vb_netplay_view_seat()==vb_netplay_authority_seat()) {
            ui_mod_items.push_back({"history.browser","System","Online save states (F7)","Host controls slots between races; every racer loads together.",RECOMP_RUNTIME_UI_ACTION,0,0,0,nullptr,0,nullptr});
        }
        if(vb_netplay_from_lobby())ui_mod_items.push_back({"session.lobby","System","Return to lobby (F10)","Leave this race and reopen the shared room.",RECOMP_RUNTIME_UI_ACTION,0,0,0,nullptr,0,nullptr});
        runtime_ui_config.extra_items = ui_mod_items.data();
        runtime_ui_config.extra_item_count = ui_mod_items.size();
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
    int network_exit=0;

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
    bool dispatched_once = false;
    uint32_t dispatch_pc = cpu.pc;
    uint64_t present_count = 0;
    /* Always-on hang watchdog (separate thread). Started here, from the main
     * thread, so it can capture this thread's stack on a freeze. */
    if(!(first_session?vb_game_host_initialize(&cpu):vb_game_host_restart(&cpu))) {
        std::fprintf(stderr,"vb-runtime: %s\n",vb_game_host_error());return 2;
    }
    vb_state_bind(&cpu, &dispatch_pc, &last_frame_cycles,
#if VB_RUNTIME_HAVE_SDL
        [](void* p) { if (p) SDL_LockAudioDevice((SDL_AudioDeviceID)(uintptr_t)p); },
        [](void* p) { if (p) SDL_UnlockAudioDevice((SDL_AudioDeviceID)(uintptr_t)p); },
        (void*)(uintptr_t)aud
#else
        nullptr, nullptr, nullptr
#endif
    );
    first_session=false;
    bool return_to_launcher=false;
#if VB_RUNTIME_HAVE_SDL
    s_lobby_requested=false;s_state_modal=false;
#endif
    uint64_t state_generation = vb_state_generation();
    if(!vb_netplay_start(&cpu,&dispatch_pc,&last_frame_cycles)) {
        std::fprintf(stderr,"vb-runtime: %s\n",vb_netplay_error());vb_netplay_stop();return 2;
    }
    // Publish the control surface only after a fresh machine, game adapter,
    // snapshot binding and network host are ready (also after a soft return).
    if(vb_debug_server_start(port,&cpu)!=0) {
        std::fprintf(stderr,"vb-runtime: failed to bind TCP port %d\n",port);
        vb_netplay_stop();vb_state_shutdown();vb_memory_shutdown();return 4;
    }
    std::printf("vb-runtime: listening on 127.0.0.1:%d\n",port);std::fflush(stdout);
    vb_debug_server_set_paused(start_paused);
    uint32_t history_frame = UINT32_MAX;
    if(!headless && !benchmark_frames && !rewind_explicit) {
        rewind_depth=vb_host_config.rewind?(unsigned)vb_host_config.rewind_depth:0;
        if(!interval_explicit)rewind_interval=(unsigned)vb_host_config.rewind_interval;
    }
#if VB_RUNTIME_HAVE_SDL
    s_history_interval=rewind_interval;
#endif
    if(vb_netplay_enabled())rewind_depth=0;
    if(rewind_depth && !vb_rewind_enable(rewind_depth)) {
        fprintf(stderr, "Unable to enable rewind: %s\n", vb_state_error());
        return 2;
    }
    if(!headless && !benchmark_frames && !vb_netplay_enabled()) {
        vb_host_config.rewind=rewind_depth!=0;
        if(rewind_depth)vb_host_config.rewind_depth=(int)rewind_depth;
        vb_host_config.rewind_interval=(int)rewind_interval;
    }
    vb_state_ui_init(state_file.c_str());
    const auto benchmark_start = std::chrono::steady_clock::now();
    const double benchmark_cpu_start = process_cpu_seconds();
    double benchmark_seconds=0, benchmark_cpu_seconds=0;
    size_t benchmark_segment=0;
    vb_watchdog_start();
    while (vb_debug_server_poll() == 0 && !sdl_quit) {
#if VB_RUNTIME_HAVE_SDL
        if(s_lobby_requested){return_to_launcher=true;break;}
#endif
        vb_state_ui_sync();
        // Debugger PC writes are applied while paused. Resume from the
        // architectural PC rather than overwriting it with the prior cache.
        dispatch_pc = cpu.pc;
        vb_watchdog_beat(VB_WD_POLL, dispatch_pc, cpu.cycles, present_count);
        if (state_generation != vb_state_generation()) {
            state_generation = vb_state_generation(); last_present_cycles = cpu.cycles;
            history_frame = UINT32_MAX;
            vb_state_ui_sync();
#if VB_RUNTIME_HAVE_SDL
            sdl_deadline = 0;
#endif
        }

        /* Axis-6 frame ring: advance the VIP-frame counter from elapsed
         * cycles (covers both the active-dispatch and HALT-idle paths, which
         * both pass through here) and record a per-frame {pc,gpr,psw}
         * fingerprint into the always-on ring. The ring is queried, never
         * armed (debug `frame` command reports the seq). Catch-up loop in
         * case a single pass spanned more than one frame period. */
        while (!vb_netplay_enabled() && cpu.cycles - last_frame_cycles >= VB_CYCLES_PER_FRAME) {
            last_frame_cycles += VB_CYCLES_PER_FRAME;
            vb_devices_end_frame(cpu.cycles);
            cpu.frame++;
            vb_game_host_end_field(&cpu);
            vb_ring_frame_record(&cpu);
        }
        if(frame_limit && cpu.frame>=frame_limit) {
            std::printf("vb-runtime: frame limit reached frame=%u native=%llu fallback=%llu\n",cpu.frame,
                        (unsigned long long)vb_execution.native_instructions,(unsigned long long)vb_execution.fallback);
            break;
        }
        if (benchmark_frames && cpu.frame >= benchmark_frames) {
            benchmark_cpu_seconds=process_cpu_seconds()-benchmark_cpu_start;
            benchmark_seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-benchmark_start).count();
            break;
        }
        if (!benchmark_route.empty()) {
            while(cpu.frame>=benchmark_route[benchmark_segment].end) ++benchmark_segment;
            vb_input_set_pad(benchmark_route[benchmark_segment].pad);
        }
        if (!vb_netplay_enabled() && history_frame != cpu.frame) {
            unsigned interval=rewind_interval;
#if VB_RUNTIME_HAVE_SDL
            interval=s_history_interval;
#endif
            if(cpu.frame%interval==0 || !vb_rewind_count()) {
                if (!vb_rewind_record(cpu.frame)) vb_stub_abort("rewind capture failed",cpu.pc,cpu.frame);
                if(!headless)vb_state_ui_record(cpu.frame);
            }
            history_frame = cpu.frame;
        }
#if VB_RUNTIME_HAVE_SDL
        if (!headless) {
            bool was_modal=vb_state_ui_mode()!=0,closed_modal=false;
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { sdl_quit = true; break; }
                int old_mode=vb_state_ui_mode();
                if (vb_state_ui_event(ev)) {closed_modal|=old_mode&&!vb_state_ui_mode();continue;}
#if defined(RECOMP_LAUNCHER)
                if (vb_runtime_ui_event(ev)) continue;
#endif
                if (ev.type == SDL_CONTROLLERDEVICEADDED ||
                    ev.type == SDL_CONTROLLERDEVICEREMOVED) {
                    gamepad_handle_device_event(ev);
                }
                if (ev.type == SDL_KEYDOWN) {
                    if(ev.key.keysym.sym==SDLK_F10 && !ev.key.repeat && vb_netplay_from_lobby()) {
                        s_lobby_requested=true;break;
                    }
                    if (ev.key.keysym.sym == SDLK_F5 && !ev.key.repeat) {
                        if(!can_pause_session())continue;
                        if(!vb_state_save_file(state_file.c_str()))
                            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Save snapshot",vb_state_error(),win);
                        continue;
                    }
                    if (ev.key.keysym.sym == SDLK_F6 && !ev.key.repeat) {
                        if(!can_pause_session())continue;
                        if(!vb_state_load_file(state_file.c_str()))
                            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR,"Load snapshot",vb_state_error(),win);
                        continue;
                    }
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
            const bool focused=(SDL_GetWindowFlags(win)&SDL_WINDOW_INPUT_FOCUS)!=0;
            const uint16_t physical=focused?(uint16_t)(pad_from_keyboard()|pad_from_gamecontroller(nullptr)):0;
            uint16_t gesture=physical&(VB_PAD_SELECT|VB_PAD_RT|VB_PAD_LT);
            if((can_pause_session() || vb_netplay_state_can_open()) && (gesture&(VB_PAD_SELECT|VB_PAD_RT))==(VB_PAD_SELECT|VB_PAD_RT) && (previous_gesture&(VB_PAD_SELECT|VB_PAD_RT))!=(VB_PAD_SELECT|VB_PAD_RT)) {
#if defined(RECOMP_LAUNCHER)
                    recomp_runtime_ui_close(s_runtime_ui);
#endif
                    vb_state_ui_action(VB_STATE_UI_SLOTS);
            } else if(can_pause_session() && (gesture&(VB_PAD_SELECT|VB_PAD_LT))==(VB_PAD_SELECT|VB_PAD_LT) && (previous_gesture&(VB_PAD_SELECT|VB_PAD_LT))!=(VB_PAD_SELECT|VB_PAD_LT)) {
#if defined(RECOMP_LAUNCHER)
                    recomp_runtime_ui_close(s_runtime_ui);
#endif
                    vb_state_ui_action(VB_STATE_UI_REWIND);
            }
            previous_gesture=gesture;
            if(vb_state_ui_mode() && s_pad) {
                int direction=0;
                int axis=SDL_GameControllerGetAxis(s_pad,SDL_CONTROLLER_AXIS_LEFTX);
                if(SDL_GameControllerGetButton(s_pad,SDL_CONTROLLER_BUTTON_DPAD_LEFT)||SDL_GameControllerGetButton(s_pad,SDL_CONTROLLER_BUTTON_DPAD_UP)||axis < -16000)direction=-1;
                if(SDL_GameControllerGetButton(s_pad,SDL_CONTROLLER_BUTTON_DPAD_RIGHT)||SDL_GameControllerGetButton(s_pad,SDL_CONTROLLER_BUTTON_DPAD_DOWN)||axis > 16000)direction=1;
                Uint32 now=SDL_GetTicks();
                if(direction && (direction!=held_menu_direction || (int32_t)(now-next_menu_repeat)>=0)) {
                    vb_state_ui_action(direction<0?VB_STATE_UI_LEFT:VB_STATE_UI_RIGHT);
                    next_menu_repeat=now+(direction!=held_menu_direction?350:90);
                }
                held_menu_direction=direction;
            } else held_menu_direction=0;
            bool modal=vb_state_ui_mode()!=0;
            if((was_modal||closed_modal)&&!modal)menu_blocked_pad|=physical;
            menu_blocked_pad&=physical;
            if(modal && !s_state_modal.exchange(true) && aud) {
                SDL_LockAudioDevice(aud);vb_vsu_discard_playback();SDL_UnlockAudioDevice(aud);
            }
            if(!modal)s_state_modal=false;
            if (sdl_quit) break;
        }
#endif
        /* The configured keyboard/controller drives a windowed game unless
         * a TCP input override is active. clear_input restores physical input. */
#if VB_RUNTIME_HAVE_SDL
        if (!headless && benchmark_route.empty() && !vb_debug_server_input_override()) {
            uint16_t controller_pad = pad_from_gamecontroller(nullptr);
            bool menu_open=vb_state_ui_mode()!=0;
#if defined(RECOMP_LAUNCHER)
            menu_open=menu_open || (s_runtime_ui && recomp_runtime_ui_is_open(s_runtime_ui));
#endif
            const bool focused=(SDL_GetWindowFlags(win)&SDL_WINDOW_INPUT_FOCUS)!=0;
            const uint16_t local_pad=menu_open || !focused?0:(uint16_t)((pad_from_keyboard() | controller_pad)&~menu_blocked_pad);
            if(vb_netplay_enabled())vb_netplay_physical_input(local_pad);else vb_input_set_pad(local_pad);
            VbGameInputs physical{};
            if(focused && !menu_open)for(unsigned seat=1;seat<vb_host_player_count();++seat)
                physical.buttons[seat]=pad_from_keyboard(seat)|s_controllers.read(seat);
            vb_game_host_physical_inputs(&physical);
        }
#endif

        if (!rom_path) {
            std::this_thread::sleep_for(2ms);
            continue;
        }

        bool simulation_paused = vb_debug_server_is_paused() != 0;
#if VB_RUNTIME_HAVE_SDL
        simulation_paused=simulation_paused || (can_pause_session() && !headless && vb_state_ui_mode());
#endif
#if defined(RECOMP_LAUNCHER) && VB_RUNTIME_HAVE_SDL
        simulation_paused = simulation_paused || (can_pause_session() && s_runtime_ui && recomp_runtime_ui_is_open(s_runtime_ui));
#endif
        if (!simulation_paused) {
        vb_watchdog_beat(VB_WD_DISPATCH, dispatch_pc, cpu.cycles, present_count);
        const int network_result=vb_netplay_enabled()?vb_netplay_poll():1;
        if(network_result<0) {
            if(network_result==-1)std::fprintf(stderr,"vb-runtime: %s\n",vb_netplay_error());
            return_to_launcher=vb_netplay_from_lobby();network_exit=network_result==-2?0:8;break;
        }
        if ((!vb_netplay_enabled() && !vb_machine_step(&cpu, &dispatch_pc, last_frame_cycles + VB_CYCLES_PER_FRAME))
            || (vb_netplay_enabled() && !network_result)) {
            if(!benchmark_frames) std::this_thread::sleep_for(1ms);
            if(!vb_netplay_enabled())continue;
        }
        if (!dispatched_once) {
            std::printf("vb-runtime: first yield at pc=0x%08X (cart is running)\n", cpu.pc);
            std::fflush(stdout); dispatched_once = true;
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
            int view_width=0,view_height=0;
            const int view_seat=vb_netplay_view_seat();
            const bool game_video=vb_game_host_video_size(view_seat,&view_width,&view_height)!=0;
            if(!game_video) {view_width=vb_renderer_present_width();view_height=stereo?VB_RT_EYE_H*2:VB_RT_EYE_H;}
            if (view_width != tex_w || view_height != tex_h) {
                SDL_Texture* resized = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                    SDL_TEXTUREACCESS_STREAMING, view_width, view_height);
                if (!resized) vb_stub_abort("cannot resize presentation texture", cpu.pc, view_width);
                SDL_DestroyTexture(tex); tex = resized; tex_w = view_width;tex_h=view_height;
                tex_storage.resize(size_t(tex_w)*tex_h);tex_pixels=tex_storage.data();
                host_capture.pixels=tex_pixels;
                host_capture.width = tex_w;
                host_capture.height=tex_h;
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
            if(game_video) {
                if(!vb_game_host_video_render(view_seat,0,tex_pixels,tex_w,tex_h))
                    vb_stub_abort("game presentation unavailable",cpu.pc,0);
            } else if (vb_renderer_active()) {
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
            if (!game_video && vb_overrides_active()) {
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

            int output_w=tex_w,output_h=tex_h;
            const uint32_t* output=tex_pixels;
            SDL_Texture* output_texture=tex;
            if(vb_state_ui_mode()) {
                output=vb_state_ui_render(&output_w,&output_h);output_texture=state_tex;
            }
            host_capture={output,output_w,output_h};
            SDL_UpdateTexture(output_texture, nullptr, output,
                              output_w * (int)sizeof(uint32_t));

            double sx = (double)win_w / output_w;
            double sy = (double)win_h / output_h;
            double s  = (sx < sy) ? sx : sy;
            SDL_Rect dst;
            dst.w = (int)(output_w * s);
            dst.h = (int)(output_h * s);
            dst.x = (win_w - dst.w) / 2;
            dst.y = (win_h - dst.h) / 2;

            SDL_SetRenderDrawColor(ren, 0, 0, 0, 0xFF);
            SDL_RenderClear(ren);
            SDL_RenderCopy(ren, output_texture, nullptr, &dst);
            SDL_RenderPresent(ren);

            /* Pace to VB_RT_FRAME_HZ — TAB skips the wait (turbo). */
            const Uint8* keys = SDL_GetKeyboardState(nullptr);
            const bool turbo = measure_runtime || (keys && keys[SDL_SCANCODE_TAB]);
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
    if (benchmark_frames) {
        if(cpu.frame!=benchmark_frames || benchmark_seconds<=0) return 7;
        uint8_t digest[32];char hash[65];vb_sha256_compute(vb_wram_data(),VB_WRAM_SIZE,digest);
        for(unsigned i=0;i<32;++i)std::sprintf(hash+2*i,"%02x",digest[i]);
        std::printf("VBRECOMP_BENCHMARK {\"frames\":%u,\"seconds\":%.9f,\"fps\":%.3f,"
            "\"process_cpu_seconds\":%.9f,\"cycles\":%llu,\"pc\":%u,\"wram_sha256\":\"%s\","
            "\"native_instructions\":%llu,\"interpreted_instructions\":%llu,\"fallback_instructions\":%llu,"
            "\"presents\":%llu,\"synthesized_audio_frames\":%llu,\"workload_scope\":\"%s\"}\n",
            cpu.frame,benchmark_seconds,cpu.frame/benchmark_seconds,benchmark_cpu_seconds,
            (unsigned long long)cpu.cycles,cpu.pc,hash,(unsigned long long)vb_execution.native_instructions,
            (unsigned long long)vb_execution.interpreted,(unsigned long long)vb_execution.fallback,
            (unsigned long long)present_count,(unsigned long long)vb_vsu_total_frames(),
            measure_runtime?"uncapped-full-runtime":"headless-core");
    }
    vb_watchdog_stop();
    vb_netplay_stop();
    vb_state_shutdown();
    vb_state_ui_shutdown();
#if defined(RECOMP_LAUNCHER) && VB_RUNTIME_HAVE_SDL
    recomp_runtime_ui_destroy(s_runtime_ui);
    s_runtime_ui = nullptr;
#endif

#if VB_RUNTIME_HAVE_SDL
    s_controllers.close();
    if (aud) { SDL_PauseAudioDevice(aud, 1); SDL_CloseAudioDevice(aud); }
    if (tex) SDL_DestroyTexture(tex);
    if (state_tex) SDL_DestroyTexture(state_tex);
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
    if(!save_ok)return 6;
    if(!return_to_launcher)return network_exit;
    if(!vb_host_end_multiplayer_sram(true)){std::puts(vb_host_error().c_str());return 6;}
    const int next=vb_host_return_to_launcher(argv[0],rom_path,headless);
    if(next<0){std::fprintf(stderr,"vb-runtime: %s\n",vb_host_error().c_str());return 8;}
    if(!next)return 0;
    }
}
