/* beetle_main.cpp — vb-beetle.exe entry point.
 *
 * Drives Beetle VB (mednafen-vb libretro core) inside an SDL window.
 * Standalone process — per Rule 14, two independent processes with the
 * same JSON wire protocol on different ports (vb-runtime on 4390,
 * vb-beetle on 4391).
 *
 * The pacing-loop shape is conceptually from psxrecomp/runtime/src/
 * beetle_main.cpp — SDL_GetPerformanceCounter deadline-based pacing,
 * TAB-for-turbo. Everything else (window dimensions, button mapping,
 * arg surface, debug port default) is VB-specific.
 */

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "vb_beetle.h"

extern "C" {
    int  vb_beetle_debug_server_start(int port);
    void vb_beetle_debug_server_stop(void);
    void vb_beetle_debug_server_poll(void);
}


/* VB native geometry. Beetle's libretro core renders into a buffer up
 * to (384*2+256) x (224*2) = 1024 x 448 to accommodate the various 3D
 * modes (side-by-side, anaglyph, over-under). Per-frame DisplayRect
 * tells us the actual valid sub-rect. We size the SDL window to the
 * default single-eye geometry scaled 2x for legibility, and let the
 * dynamic upload + RenderCopy handle whatever Beetle emits. */
static constexpr int VB_SDL_SCALE = 2;
static constexpr int VB_BASE_W = 384;
static constexpr int VB_BASE_H = 224;
static constexpr int VB_MAX_FB_W = 1024;
static constexpr int VB_MAX_FB_H = 448;
static constexpr double VB_FRAME_HZ = 50.27;


static uint16_t pad_from_keyboard(void) {
    /* All bits set = nothing pressed. The VB pad is active-low at
     * the hardware level. We accumulate "pressed" bits as cleared
     * bits.
     *
     * Key map (mirrors the obvious keyboard layout for a two-DPad
     * controller):
     *   Left DPad : Arrow keys
     *   Right DPad: WASD
     *   A / B     : X / Z
     *   L / R     : Q / E
     *   Start / Select : Return / Right Shift
     */
    const Uint8* keys = SDL_GetKeyboardState(nullptr);
    uint16_t pad = 0xFFFF;
    auto press = [&pad](int bit) { pad &= (uint16_t)~(1u << bit); };

    if (keys[SDL_SCANCODE_UP])      press(VB_BEETLE_PAD_LDPAD_UP);
    if (keys[SDL_SCANCODE_DOWN])    press(VB_BEETLE_PAD_LDPAD_DOWN);
    if (keys[SDL_SCANCODE_LEFT])    press(VB_BEETLE_PAD_LDPAD_LEFT);
    if (keys[SDL_SCANCODE_RIGHT])   press(VB_BEETLE_PAD_LDPAD_RIGHT);

    if (keys[SDL_SCANCODE_W])       press(VB_BEETLE_PAD_RDPAD_UP);
    if (keys[SDL_SCANCODE_S])       press(VB_BEETLE_PAD_RDPAD_DOWN);
    if (keys[SDL_SCANCODE_A])       press(VB_BEETLE_PAD_RDPAD_LEFT);
    if (keys[SDL_SCANCODE_D])       press(VB_BEETLE_PAD_RDPAD_RIGHT);

    if (keys[SDL_SCANCODE_X])       press(VB_BEETLE_PAD_A);
    if (keys[SDL_SCANCODE_Z])       press(VB_BEETLE_PAD_B);
    if (keys[SDL_SCANCODE_Q])       press(VB_BEETLE_PAD_L_TRIGGER);
    if (keys[SDL_SCANCODE_E])       press(VB_BEETLE_PAD_R_TRIGGER);

    if (keys[SDL_SCANCODE_RETURN])  press(VB_BEETLE_PAD_START);
    if (keys[SDL_SCANCODE_RSHIFT])  press(VB_BEETLE_PAD_SELECT);
    return pad;
}


static int load_file(const char* path, std::vector<uint8_t>& out) {
    FILE* f = std::fopen(path, "rb");
    if (!f) return -1;
    std::fseek(f, 0, SEEK_END);
    long sz = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (sz <= 0) { std::fclose(f); return -2; }
    out.resize((size_t)sz);
    size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return (got == out.size()) ? 0 : -3;
}


int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IOLBF, 0);

    const char* rom_path = nullptr;
    int debug_port = 4391;
    bool headless = false;

    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--rom") && i + 1 < argc) {
            rom_path = argv[++i];
        } else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
            debug_port = std::atoi(argv[++i]);
        } else if (!std::strcmp(argv[i], "--headless")) {
            /* No SDL window, just emulate + serve TCP. Useful for
             * CI where no display is available. */
            headless = true;
        } else if (!std::strcmp(argv[i], "--help") || !std::strcmp(argv[i], "-h")) {
            std::printf(
                "Usage: %s --rom PATH [--port N] [--headless]\n"
                "\n"
                "  --rom PATH     Virtual Boy cart to load\n"
                "  --port N       TCP debug port (default 4391)\n"
                "  --headless     no SDL window; emulate + TCP only\n",
                argv[0]);
            return 0;
        } else if (argv[i][0] != '-') {
            rom_path = argv[i];
        }
    }
    if (!rom_path) {
        std::fprintf(stderr, "vb-beetle: --rom is required\n");
        return 1;
    }

    std::vector<uint8_t> rom;
    if (load_file(rom_path, rom) != 0) {
        std::fprintf(stderr, "vb-beetle: failed to read %s\n", rom_path);
        return 1;
    }

    if (vb_beetle_init(rom.data(), rom.size()) != 0) {
        std::fprintf(stderr, "vb-beetle: vb_beetle_init failed\n");
        return 1;
    }

    SDL_Window* win = nullptr;
    SDL_Renderer* ren = nullptr;
    SDL_Texture* tex = nullptr;

    if (!headless) {
        if (SDL_Init(SDL_INIT_VIDEO) != 0) {
            std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
            return 1;
        }
        win = SDL_CreateWindow(
            "vb-beetle — Beetle VB",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            VB_BASE_W * VB_SDL_SCALE, VB_BASE_H * VB_SDL_SCALE,
            SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
        if (!win) {
            std::fprintf(stderr, "SDL_CreateWindow failed: %s\n",
                         SDL_GetError());
            vb_beetle_shutdown();
            return 1;
        }
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl");
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
        if (!ren) {
            ren = SDL_CreateRenderer(win, -1, 0);  /* fall back to any */
        }
        if (!ren) {
            std::fprintf(stderr, "SDL_CreateRenderer failed: %s\n",
                         SDL_GetError());
            SDL_DestroyWindow(win);
            SDL_Quit();
            vb_beetle_shutdown();
            return 1;
        }
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                SDL_TEXTUREACCESS_STREAMING,
                                VB_MAX_FB_W, VB_MAX_FB_H);
        if (!tex) {
            std::fprintf(stderr, "SDL_CreateTexture failed: %s\n",
                         SDL_GetError());
            SDL_DestroyRenderer(ren);
            SDL_DestroyWindow(win);
            SDL_Quit();
            vb_beetle_shutdown();
            return 1;
        }
    }

    if (vb_beetle_debug_server_start(debug_port) != 0) {
        std::fprintf(stderr, "vb-beetle: TCP server failed on port %d\n",
                     debug_port);
        /* Non-fatal — keep going so the window still renders. */
    }

    /* Wall-clock pacing to VB native 50.27 Hz. TAB → unlocked turbo. */
    const double frame_ms = 1000.0 / VB_FRAME_HZ;
    Uint64 freq = SDL_GetPerformanceFrequency();
    Uint64 period = (Uint64)((double)freq * (frame_ms / 1000.0));
    Uint64 deadline = 0;

    for (;;) {
        if (!headless) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) goto shutdown;
                if (ev.type == SDL_KEYDOWN
                        && ev.key.keysym.sym == SDLK_ESCAPE) goto shutdown;
            }
        }

        vb_beetle_debug_server_poll();

        uint16_t pad = headless ? vb_beetle_get_pad() : pad_from_keyboard();
        vb_beetle_run_frame(pad);

        if (!headless) {
            const uint32_t* fb = nullptr;
            unsigned w = 0, h = 0;
            if (vb_beetle_get_framebuffer(&fb, &w, &h) && fb && w && h) {
                if (w > VB_MAX_FB_W) w = VB_MAX_FB_W;
                if (h > VB_MAX_FB_H) h = VB_MAX_FB_H;
                /* Beetle pitches its surface as VB_MAX_FB_W (1024) px
                 * regardless of the visible width; safe to upload only
                 * the visible w*h with explicit pitch. */
                SDL_Rect dirty = { 0, 0, (int)w, (int)h };
                SDL_UpdateTexture(tex, &dirty, fb, (int)(w * sizeof(uint32_t)));
                SDL_Rect dst;
                int win_w = 0, win_h = 0;
                SDL_GetRendererOutputSize(ren, &win_w, &win_h);
                /* Letterbox to preserve aspect. */
                double sx = (double)win_w / w;
                double sy = (double)win_h / h;
                double s  = (sx < sy) ? sx : sy;
                dst.w = (int)(w * s);
                dst.h = (int)(h * s);
                dst.x = (win_w - dst.w) / 2;
                dst.y = (win_h - dst.h) / 2;
                SDL_SetRenderDrawColor(ren, 0, 0, 0, 0xFF);
                SDL_RenderClear(ren);
                SDL_RenderCopy(ren, tex, &dirty, &dst);
                SDL_RenderPresent(ren);
            }
        }

        /* Pacing — TAB skips the wait (turbo). */
        if (!headless) {
            const Uint8* keys = SDL_GetKeyboardState(nullptr);
            if (keys && keys[SDL_SCANCODE_TAB]) continue;
        }
        Uint64 now = SDL_GetPerformanceCounter();
        if (deadline == 0 || now >= deadline + period * 3) {
            deadline = now + period;
        } else {
            while (SDL_GetPerformanceCounter() < deadline) {
                Uint64 left = deadline - SDL_GetPerformanceCounter();
                Uint64 ms = (left * 1000) / freq;
                if (ms >= 2) SDL_Delay((Uint32)(ms - 1));
                else break;
            }
            while (SDL_GetPerformanceCounter() < deadline) { /* spin */ }
            deadline += period;
        }
    }

shutdown:
    vb_beetle_debug_server_stop();
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    if (!headless) SDL_Quit();
    vb_beetle_shutdown();
    return 0;
}
