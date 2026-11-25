#include "headless_capture.h"

#include <cstring>

int main(int argc, char** argv) {
    if (argc != 4) return 1;
    VbCaptureConfig config{};
    config.out_dir = std::strcmp(argv[1], "-") ? argv[1] : nullptr;
    config.wav_path = std::strcmp(argv[2], "-") ? argv[2] : nullptr;
    config.input_script = std::strcmp(argv[3], "-") ? argv[3] : nullptr;
    config.width = 4;
    config.height = 2;
    config.from = 2;
    config.to = 4;
    config.every = 2;
    if (!vb_capture_start(&config)) return 1;
    for (uint32_t frame = 1; frame <= 5; ++frame) {
        const uint32_t pixels[] = {0xff102030u, 0xff405060u};
        const int16_t samples[] = {(int16_t)frame, (int16_t)(100 + frame)};
        vb_capture_present(pixels, 2, 1, 0, 0, 4, 2, frame);
        vb_capture_audio(samples, 1, frame);
    }
    vb_capture_finish();
    return 0;
}
