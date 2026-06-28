/* beetle_libretro.cpp — vb-beetle libretro driver.
 *
 * Hosts mednafen-vb (Beetle VB) as a static libretro core: registers
 * the libretro callbacks Beetle expects, calls retro_init /
 * retro_load_game / retro_run on its behalf, and exposes a small C API
 * (see runtime/include/vb_beetle.h) for SDL + the debug server.
 *
 * What we deliberately do NOT do here:
 *   - port psxrecomp's SIO/wtrace/fntrace rings (PSX-specific, and the
 *     analogous VB instrumentation is the VIP/VSU/timer rings on the
 *     runtime side, not over here)
 *   - reach into Beetle's V810 register file (mednafen-vb does not
 *     expose it externally; a future patch can lift this restriction
 *     when there's a concrete need)
 *   - play audio (Beetle hands us PCM samples; for the P2.5 visual
 *     milestone we capture them and immediately drop them on the
 *     floor — audio output is a P6 concern)
 *
 * Why static accessors and not member functions: the debug server is
 * C, libretro is C; bridging them through C++ would just add name-
 * mangling noise. The compilation unit is C++ only because libretro.h
 * itself uses some C++-isms in adjacent mednafen headers.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "libretro.h"
#include "vb_beetle.h"


namespace {

/* libretro-symbol entry points we drive (linked from libmednafen_vb.a) */
extern "C" {
    void   retro_init(void);
    void   retro_deinit(void);
    void   retro_run(void);
    bool   retro_load_game(const struct retro_game_info* info);
    void   retro_unload_game(void);
    void   retro_get_system_info(struct retro_system_info* info);
    void   retro_get_system_av_info(struct retro_system_av_info* info);
    void   retro_set_environment(retro_environment_t cb);
    void   retro_set_video_refresh(retro_video_refresh_t cb);
    void   retro_set_audio_sample(retro_audio_sample_t cb);
    void   retro_set_audio_sample_batch(retro_audio_sample_batch_t cb);
    void   retro_set_input_poll(retro_input_poll_t cb);
    void   retro_set_input_state(retro_input_state_t cb);
    unsigned retro_api_version(void);
    void   retro_set_controller_port_device(unsigned port, unsigned device);
    void*  retro_get_memory_data(unsigned id);
    size_t retro_get_memory_size(unsigned id);
}

/* ---- captured state ---- */
std::vector<uint32_t> s_framebuffer;
unsigned s_fb_w = 0;
unsigned s_fb_h = 0;
bool     s_have_frame = false;

uint16_t s_pad = 0xFFFF;          /* active-low; all released */
std::atomic<uint32_t> s_frame_count{0};
bool     s_loaded = false;

/* Cart ROM lifetime is owned by the caller; we keep a copy of the
 * pointer + size for `vb_beetle_read_memory` so the read can cover
 * the cart-ROM bank without poking Beetle's internals.
 *
 * Beetle keeps its OWN copy of the bytes (libretro convention with
 * need_fullpath=false), so the caller can free its buffer the moment
 * vb_beetle_init returns — but we hold the original anyway for read
 * symmetry with our own runtime. */
const uint8_t* s_rom_ptr = nullptr;
size_t         s_rom_size = 0;

/* ---- always-on audio capture ring ----
 * Beetle delivers interleaved S16 stereo PCM via audio_batch_cb every
 * retro_run(). Pre-accuracy work discarded it; we now record every
 * frame from boot into a power-of-two ring so the accuracy harness can
 * pull a window by absolute index (the oracle side of the recomp's VSU
 * capture). 1<<20 frames ≈ 23.8 s @ 44.1 kHz = 4 MiB static. */
constexpr size_t   kAudioRingFrames = 1u << 20;
constexpr size_t   kAudioRingMask   = kAudioRingFrames - 1;
constexpr unsigned kAudioRateHz     = 44100;
int16_t  s_audio_ring[kAudioRingFrames * 2];  /* interleaved L,R */
uint64_t s_audio_total = 0;                   /* frames ever written */

/* Beetle requests pixel format via the environment callback. We always
 * say "XRGB8888" because libretro-cpp's WANT_32BPP build path expects
 * that and the framebuffer width/pitch are 32 bpp. */
retro_pixel_format s_pixel_fmt = RETRO_PIXEL_FORMAT_XRGB8888;


/* ---- libretro callback impls ---- */

bool environ_cb(unsigned cmd, void* data) {
    switch (cmd) {
    case RETRO_ENVIRONMENT_GET_LOG_INTERFACE: {
        /* Beetle uses log_cb if available; we suppress all log output
         * because our runtime prints exactly one allowed line ever
         * (the stub_abort banner). Force callers to ignore log
         * messages by returning false. */
        return false;
    }
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
        if (!data) return false;
        s_pixel_fmt = *(const retro_pixel_format*)data;
        return s_pixel_fmt == RETRO_PIXEL_FORMAT_XRGB8888;
    }
    case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
    case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY: {
        /* No save dir; mednafen-vb gracefully runs without one. */
        if (data) *(const char**)data = ".";
        return true;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE: {
        /* Return "no value set" — Beetle picks defaults for all of
         * its config knobs (3D mode, color, etc.). */
        if (data) ((retro_variable*)data)->value = nullptr;
        return false;
    }
    case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE: {
        if (data) *(bool*)data = false;
        return true;
    }
    case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS: {
        /* Tell Beetle we don't support input bitmasks; it falls back
         * to per-button input_state queries which is what our
         * input_state_cb implements. */
        return false;
    }
    default:
        return false;
    }
}

void video_refresh_cb(const void* data, unsigned width, unsigned height,
                      size_t pitch) {
    if (!data || !width || !height) {
        /* Beetle's libretro.cpp can hand us a null pointer for
         * dupe-frame frames (RETRO_HW_FRAME_BUFFER_VALID-style
         * convention); preserve the prior framebuffer. */
        return;
    }
    s_fb_w = width;
    s_fb_h = height;
    s_framebuffer.resize((size_t)width * height);

    const uint8_t* src = (const uint8_t*)data;
    for (unsigned y = 0; y < height; ++y) {
        const uint32_t* row = (const uint32_t*)(src + y * pitch);
        uint32_t* dst = s_framebuffer.data() + (size_t)y * width;
        for (unsigned x = 0; x < width; ++x) {
            /* Beetle's XRGB8888 alpha byte is undefined — force opaque
             * so an SDL renderer that respects alpha doesn't black
             * the frame. */
            dst[x] = row[x] | 0xFF000000u;
        }
    }
    s_have_frame = true;
}

void audio_sample_cb(int16_t /*l*/, int16_t /*r*/) {
    /* mednafen-vb delivers audio exclusively through the batch callback
     * (libretro.cpp audio_batch_cb), so this per-sample entry is never
     * invoked. Left as a discard to avoid double-counting the ring. */
}

size_t audio_batch_cb(const int16_t* data, size_t frames) {
    /* Always-on capture: record every interleaved (L,R) frame into the
     * ring from boot. The accuracy harness pulls a window by absolute
     * index via vb_beetle_audio_read_abs — a ring QUERY, never an armed
     * capture (global ring-buffer rule). */
    if (data) {
        for (size_t i = 0; i < frames; ++i) {
            size_t slot = (size_t)(s_audio_total & kAudioRingMask);
            s_audio_ring[slot * 2 + 0] = data[i * 2 + 0];
            s_audio_ring[slot * 2 + 1] = data[i * 2 + 1];
            s_audio_total++;
        }
    }
    return frames;
}

void input_poll_cb_impl(void) {
    /* No-op: we set s_pad before each retro_run via vb_beetle_run_frame. */
}

int16_t input_state_cb_impl(unsigned port, unsigned device,
                            unsigned /*index*/, unsigned id) {
    if (port != 0 || device != RETRO_DEVICE_JOYPAD) return 0;

    /* Translate the libretro joypad id Beetle queries into the
     * corresponding bit in our pad mask. Beetle's own pad order
     * (see libretro.cpp `update_input` around line 2361) defines the
     * mapping; we mirror it exactly so the resulting VB-side
     * `s_PadData` byte matches what a libretro frontend would build. */
    auto bit_for = [](unsigned libretro_id) -> int {
        switch (libretro_id) {
        case RETRO_DEVICE_ID_JOYPAD_A:      return VB_BEETLE_PAD_A;
        case RETRO_DEVICE_ID_JOYPAD_B:      return VB_BEETLE_PAD_B;
        case RETRO_DEVICE_ID_JOYPAD_R:      return VB_BEETLE_PAD_R_TRIGGER;
        case RETRO_DEVICE_ID_JOYPAD_L:      return VB_BEETLE_PAD_L_TRIGGER;
        case RETRO_DEVICE_ID_JOYPAD_L2:     return VB_BEETLE_PAD_RDPAD_UP;
        case RETRO_DEVICE_ID_JOYPAD_R3:     return VB_BEETLE_PAD_RDPAD_RIGHT;
        case RETRO_DEVICE_ID_JOYPAD_RIGHT:  return VB_BEETLE_PAD_LDPAD_RIGHT;
        case RETRO_DEVICE_ID_JOYPAD_LEFT:   return VB_BEETLE_PAD_LDPAD_LEFT;
        case RETRO_DEVICE_ID_JOYPAD_DOWN:   return VB_BEETLE_PAD_LDPAD_DOWN;
        case RETRO_DEVICE_ID_JOYPAD_UP:     return VB_BEETLE_PAD_LDPAD_UP;
        case RETRO_DEVICE_ID_JOYPAD_START:  return VB_BEETLE_PAD_START;
        case RETRO_DEVICE_ID_JOYPAD_SELECT: return VB_BEETLE_PAD_SELECT;
        case RETRO_DEVICE_ID_JOYPAD_R2:     return VB_BEETLE_PAD_RDPAD_LEFT;
        case RETRO_DEVICE_ID_JOYPAD_L3:     return VB_BEETLE_PAD_RDPAD_DOWN;
        default: return -1;
        }
    };
    int bit = bit_for(id);
    if (bit < 0) return 0;
    /* s_pad is active-low: a clear bit means pressed. libretro expects
     * 1 for pressed. */
    return ((s_pad >> bit) & 1) ? 0 : 1;
}

}  /* anon namespace */


/* ============================================================== *
 *                      Public C API                                *
 * ============================================================== */

extern "C" {

int vb_beetle_init(const uint8_t* rom_bytes, size_t rom_size) {
    if (s_loaded) return -1;
    if (!rom_bytes || rom_size == 0) return -2;

    s_rom_ptr  = rom_bytes;
    s_rom_size = rom_size;

    /* Register callbacks BEFORE retro_init so Beetle picks them up.
     * libretro's contract: set_* callbacks must be registered before
     * retro_init; retro_init may call into environ_cb. */
    retro_set_environment(environ_cb);
    retro_set_video_refresh(video_refresh_cb);
    retro_set_audio_sample(audio_sample_cb);
    retro_set_audio_sample_batch(audio_batch_cb);
    retro_set_input_poll(input_poll_cb_impl);
    retro_set_input_state(input_state_cb_impl);

    retro_init();

    struct retro_game_info game;
    std::memset(&game, 0, sizeof(game));
    game.path = nullptr;
    game.data = rom_bytes;
    game.size = rom_size;
    game.meta = nullptr;
    if (!retro_load_game(&game)) {
        retro_deinit();
        return -3;
    }

    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);

    s_loaded = true;
    s_frame_count.store(0);
    s_audio_total = 0;
    return 0;
}

void vb_beetle_shutdown(void) {
    if (!s_loaded) return;
    retro_unload_game();
    retro_deinit();
    s_loaded = false;
    s_framebuffer.clear();
    s_fb_w = 0;
    s_fb_h = 0;
    s_have_frame = false;
}

void vb_beetle_run_frame(uint16_t pad) {
    if (!s_loaded) return;
    s_pad = pad;
    retro_run();
    s_frame_count.fetch_add(1, std::memory_order_relaxed);
}

int vb_beetle_get_framebuffer(const uint32_t** out_pixels,
                              unsigned* out_w, unsigned* out_h) {
    if (!s_have_frame || s_framebuffer.empty()) return 0;
    if (out_pixels) *out_pixels = s_framebuffer.data();
    if (out_w)      *out_w      = s_fb_w;
    if (out_h)      *out_h      = s_fb_h;
    return 1;
}

uint16_t vb_beetle_get_pad(void)         { return s_pad; }
void     vb_beetle_set_pad(uint16_t pad) { s_pad = pad; }
uint32_t vb_beetle_frame_count(void) {
    return s_frame_count.load(std::memory_order_relaxed);
}
size_t   vb_beetle_rom_size(void)        { return s_rom_size; }

uint64_t vb_beetle_audio_total(void)     { return s_audio_total; }
unsigned vb_beetle_audio_rate(void)      { return kAudioRateHz; }

size_t vb_beetle_audio_read_abs(uint64_t start_abs, int16_t* dst,
                                size_t max_frames,
                                uint64_t* out_head_abs,
                                uint64_t* out_resident_lo) {
    const uint64_t head = s_audio_total;
    /* Frame i is overwritten at write (i + capacity); anything >=
     * head - capacity is still in its slot. */
    const uint64_t resident_lo =
        (head > (uint64_t)kAudioRingFrames) ? head - (uint64_t)kAudioRingFrames : 0;
    if (out_head_abs)    *out_head_abs    = head;
    if (out_resident_lo) *out_resident_lo = resident_lo;
    if (!dst || !max_frames) return 0;

    uint64_t cur = start_abs < resident_lo ? resident_lo : start_abs;
    size_t produced = 0;
    while (produced < max_frames && cur < head) {
        size_t slot = (size_t)(cur & kAudioRingMask);
        dst[produced * 2 + 0] = s_audio_ring[slot * 2 + 0];
        dst[produced * 2 + 1] = s_audio_ring[slot * 2 + 1];
        produced++;
        cur++;
    }
    return produced;
}

size_t vb_beetle_read_memory(uint32_t va, uint8_t* out, size_t len) {
    if (!out || !len) return 0;
    if (!s_loaded) return 0;

    size_t filled = 0;
    while (filled < len) {
        uint32_t cur = (va + (uint32_t)filled) & 0xFFFFFFFF;
        uint32_t phys = cur & 0x07FFFFFF;
        uint8_t* src = nullptr;
        size_t available = 0;

        if (phys >= 0x05000000 && phys < 0x06000000) {
            /* WRAM — 64 KB mirrored throughout bank 5. */
            void* wram = retro_get_memory_data(RETRO_MEMORY_SYSTEM_RAM);
            size_t wsz = retro_get_memory_size(RETRO_MEMORY_SYSTEM_RAM);
            if (!wram || !wsz) break;
            uint32_t off = (phys - 0x05000000) % (uint32_t)wsz;
            src = (uint8_t*)wram + off;
            available = wsz - off;
        } else if (phys >= 0x06000000 && phys < 0x07000000) {
            /* GPRAM (cart RAM) — present only on save-RAM carts. */
            void* gpram = retro_get_memory_data(RETRO_MEMORY_SAVE_RAM);
            size_t gsz = retro_get_memory_size(RETRO_MEMORY_SAVE_RAM);
            if (!gpram || !gsz) break;
            uint32_t off = (phys - 0x06000000) % (uint32_t)gsz;
            src = (uint8_t*)gpram + off;
            available = gsz - off;
        } else if (phys >= 0x07000000 && phys < 0x08000000) {
            /* Cart ROM mirror — we hold the original bytes ourselves
             * so the read works even if Beetle copied the cart into
             * an internal allocation we can't reach. */
            if (!s_rom_ptr || !s_rom_size) break;
            uint32_t off = (phys - 0x07000000) % (uint32_t)s_rom_size;
            src = (uint8_t*)s_rom_ptr + off;
            available = s_rom_size - off;
        } else {
            /* VIP / VSU / pad / timer / etc. — not exposed externally
             * by mednafen-vb without an upstream patch. Short-read
             * cleanly so the caller can decide what to do. */
            break;
        }

        size_t take = len - filled;
        if (take > available) take = available;
        std::memcpy(out + filled, src, take);
        filled += take;
    }
    return filled;
}

/* VIP state — backed by Beetle's VIP_GetRegister. The mednafen-vb
 * core exposes only the writable/readable register bank via this
 * accessor (vip.h:VIP_GSREG_*). Internal state-machine vars and the
 * synthesised DPSTTS/XPSTTS read-only registers stay opaque without
 * a beetle-vb patch; for P4-B verification we diff the writable bank
 * only, which is sufficient to confirm the cart's register-
 * programming matches between our runtime and the oracle. */
extern "C" {
    /* From beetle-vb/mednafen/vb/vip.h. We forward-declare rather
     * than #include the C++ mednafen header to keep this driver's
     * compile boundary clean. */
    uint32_t VIP_GetRegister(unsigned id, char* special, uint32_t special_len);

    enum {
        VBB_VIP_GSREG_IPENDING = 0,
        VBB_VIP_GSREG_IENABLE  = 1,
        VBB_VIP_GSREG_DPCTRL   = 2,
        VBB_VIP_GSREG_BRTA     = 3,
        VBB_VIP_GSREG_BRTB     = 4,
        VBB_VIP_GSREG_BRTC     = 5,
        VBB_VIP_GSREG_REST     = 6,
        VBB_VIP_GSREG_FRMCYC   = 7,
        VBB_VIP_GSREG_XPCTRL   = 8,
        VBB_VIP_GSREG_SPT0     = 9,
        VBB_VIP_GSREG_GPLT0    = 13,
        VBB_VIP_GSREG_JPLT0    = 17,
        VBB_VIP_GSREG_BKCOL    = 21,
    };
}

void vb_beetle_get_vip_state(vb_beetle_vip_state* out) {
    if (!out) return;
    out->intpnd = (uint16_t)VIP_GetRegister(VBB_VIP_GSREG_IPENDING, nullptr, 0);
    out->intenb = (uint16_t)VIP_GetRegister(VBB_VIP_GSREG_IENABLE,  nullptr, 0);
    out->dpctrl = (uint16_t)VIP_GetRegister(VBB_VIP_GSREG_DPCTRL,   nullptr, 0);
    out->xpctrl = (uint16_t)VIP_GetRegister(VBB_VIP_GSREG_XPCTRL,   nullptr, 0);
    out->frmcyc = (uint16_t)VIP_GetRegister(VBB_VIP_GSREG_FRMCYC,   nullptr, 0);
    out->bkcol  = (uint16_t)VIP_GetRegister(VBB_VIP_GSREG_BKCOL,    nullptr, 0);
    out->brta   = (uint8_t) VIP_GetRegister(VBB_VIP_GSREG_BRTA,     nullptr, 0);
    out->brtb   = (uint8_t) VIP_GetRegister(VBB_VIP_GSREG_BRTB,     nullptr, 0);
    out->brtc   = (uint8_t) VIP_GetRegister(VBB_VIP_GSREG_BRTC,     nullptr, 0);
    out->rest   = (uint8_t) VIP_GetRegister(VBB_VIP_GSREG_REST,     nullptr, 0);
    for (int i = 0; i < 4; ++i) {
        out->spt[i]  = (uint16_t)VIP_GetRegister(VBB_VIP_GSREG_SPT0  + i, nullptr, 0);
        out->gplt[i] = (uint16_t)VIP_GetRegister(VBB_VIP_GSREG_GPLT0 + i, nullptr, 0);
        out->jplt[i] = (uint16_t)VIP_GetRegister(VBB_VIP_GSREG_JPLT0 + i, nullptr, 0);
    }
}

}  /* extern "C" */
