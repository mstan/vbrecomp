/* vb_beetle.h — C accessor surface exposed by the Beetle VB libretro
 * driver to the rest of vb-beetle.exe (SDL main + TCP debug server).
 *
 * The driver lives in `beetle_libretro.cpp`. Everything below is a
 * plain C function so the TCP server (debug_server.c) can call into
 * it without C++ name-mangling games.
 *
 * Lifecycle:
 *     vb_beetle_init        load cart bytes, init libretro core
 *     vb_beetle_run_frame   one retro_run() invocation
 *     vb_beetle_shutdown    retro_deinit() + free
 *
 * State accessors (used by SDL + debug server):
 *     vb_beetle_get_framebuffer  latest video_refresh frame
 *     vb_beetle_get_pad          current 16-bit pad mask
 *     vb_beetle_set_pad          set the pad from main()
 *     vb_beetle_frame_count      monotonic frame counter
 *     vb_beetle_read_memory      WRAM / SAVE_RAM / cart ROM read
 *
 * Memory model exposed via vb_beetle_read_memory:
 *   - 0x05000000..0x0500FFFF -> WRAM (64 KB; mirrored further is
 *                               handled by VA-modulo on the caller
 *                               side, e.g. the debug command applies
 *                               the V810 27-bit phys mask).
 *   - 0x06000000..0x06FFFFFF -> GPRAM if present (size depends on cart)
 *   - 0x07000000..0x07FFFFFF -> cart ROM (mirrors via modulo)
 *
 * Anything else returns 0 — the runtime-side debug client treats
 * "no bytes" as a diagnostic, not a fatal. */
#ifndef VB_BEETLE_H
#define VB_BEETLE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns 0 on success, negative on failure. `rom_bytes` must remain
 * valid for the lifetime of the process — Beetle keeps a pointer into
 * it. */
int  vb_beetle_init(const uint8_t* rom_bytes, size_t rom_size);
void vb_beetle_shutdown(void);

/* Run one emulated frame. `pad` is the active-low pad bitmap (see
 * `vb_beetle_pad_bits` below for the bit layout). */
void vb_beetle_run_frame(uint16_t pad);

/* Latest video frame. Returns 1 if a frame has been captured since
 * init; pixels are 32-bit XRGB8888 native-endian, `*w * *h` total. */
int  vb_beetle_get_framebuffer(const uint32_t** out_pixels,
                               unsigned* out_w, unsigned* out_h);

/* Pad mask currently in effect (the value last passed to
 * vb_beetle_run_frame). Active-low: a clear bit means pressed. */
uint16_t vb_beetle_get_pad(void);

/* Set the pad value the next retro_run will see. The debug server
 * uses this for the (P3+) `set_input` override. */
void vb_beetle_set_pad(uint16_t pad);

/* Total emulated frames since init. */
uint32_t vb_beetle_frame_count(void);

/* Read `len` bytes starting at virtual address `va` into `out`.
 * Returns the number of bytes actually filled — gaps in the memory
 * map produce a short read, never a crash. */
size_t vb_beetle_read_memory(uint32_t va, uint8_t* out, size_t len);

/* Size of the cart ROM in bytes. */
size_t vb_beetle_rom_size(void);

/* VIP state snapshot, for cross-process diff against vb-runtime's
 * vip_state command. Only the fields exposed by Beetle's
 * VIP_GetRegister(VIP_GSREG_*) — internal state-machine vars
 * (Column, DisplayRegion, DrawingBlock, etc.) and the synthesised
 * DPSTTS/XPSTTS values require a beetle-vb patch to expose. */
typedef struct vb_beetle_vip_state {
    uint16_t intpnd;
    uint16_t intenb;
    uint16_t dpctrl;
    uint16_t xpctrl;
    uint16_t frmcyc;
    uint16_t bkcol;
    uint8_t  brta, brtb, brtc, rest;
    uint16_t spt[4];
    uint16_t gplt[4];
    uint16_t jplt[4];
} vb_beetle_vip_state;

void vb_beetle_get_vip_state(vb_beetle_vip_state* out);

/* libretro device-id bit indices that Beetle expects in the
 * input_state callback. The runtime side composes a pad value by OR'ing
 * (1 << VB_BEETLE_PAD_<NAME>) for each pressed button. We translate
 * this to the libretro device ID in the input_state callback. */
typedef enum {
    VB_BEETLE_PAD_A = 0,
    VB_BEETLE_PAD_B,
    VB_BEETLE_PAD_R_TRIGGER,
    VB_BEETLE_PAD_L_TRIGGER,
    VB_BEETLE_PAD_RDPAD_UP,
    VB_BEETLE_PAD_RDPAD_RIGHT,
    VB_BEETLE_PAD_LDPAD_RIGHT,
    VB_BEETLE_PAD_LDPAD_LEFT,
    VB_BEETLE_PAD_LDPAD_DOWN,
    VB_BEETLE_PAD_LDPAD_UP,
    VB_BEETLE_PAD_START,
    VB_BEETLE_PAD_SELECT,
    VB_BEETLE_PAD_RDPAD_LEFT,
    VB_BEETLE_PAD_RDPAD_DOWN,
} vb_beetle_pad_bits;

#ifdef __cplusplus
}
#endif

#endif /* VB_BEETLE_H */
