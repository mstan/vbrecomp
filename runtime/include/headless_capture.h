/* Headless capture for the Virtual Boy runtime: record exactly what the
 * window presents, with no window, audio device or desktop input.
 *
 *   --capture-out DIR       DIR/frame_NNNNNN.png of the presented picture
 *   --capture-size WxH      drawable size a window of that size would have
 *                           (default 1920x1080; 2560x1080 for 21:9)
 *   --capture-from/-to N, --capture-every K
 *   --capture-wav FILE      mixed VSU output, 44.1 kHz stereo, from --capture-from
 *   --input-script FILE     `<frame> [P:]BUTTONS` (P = seat 1..4; `-` releases;
 *                           A B L R START SELECT UP DOWN LEFT RIGHT and the
 *                           explicit LUP LDOWN LLEFT LRIGHT RUP RDOWN RLEFT RRIGHT)
 *   --exit-at-frame N
 *
 * Capture mode selects SDL's dummy drivers (so SDL_CreateWindow succeeds
 * without anything appearing), runs the real present path unpaced, and pulls
 * audio once per presented frame. */
#ifndef VB_HEADLESS_CAPTURE_H
#define VB_HEADLESS_CAPTURE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VbCaptureConfig {
    const char* out_dir;      /* NULL = no frames */
    const char* wav_path;     /* NULL = no audio */
    const char* input_script; /* NULL = none */
    int width, height;        /* 0 = 1920x1080 */
    long from, to;            /* -1 = unset */
    int every;                /* >= 1 */
} VbCaptureConfig;

/* Starts capture mode (selects SDL dummy drivers: call before SDL_Init).
 * Returns 1 when frames or audio were requested. */
int vb_capture_start(const VbCaptureConfig* config);
int vb_capture_enabled(void);
int vb_capture_has_input(void);
void vb_capture_size(int* width, int* height);
int vb_capture_frame_wanted(uint32_t frame);

/* `argb` is the presented w*h picture; (dx,dy,dw,dh) is where the window path
 * places it inside the canvas. Nearest sampling, black elsewhere. */
void vb_capture_present(const uint32_t* argb, int w, int h,
                        int dx, int dy, int dw, int dh, uint32_t frame);

/* Samples the VSU should deliver for presented frame `frame` (44.1 kHz). */
int vb_capture_audio_frames(uint32_t frame);
void vb_capture_audio(const int16_t* stereo, int frames, uint32_t frame);

/* Pad word (VB_PAD_* bits, PRESENT set) for seat 0..3 at `frame`. */
uint16_t vb_capture_pad(unsigned seat, uint32_t frame);

void vb_capture_finish(void);

#ifdef __cplusplus
}
#endif
#endif
