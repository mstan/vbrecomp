#include "stereo.h"
#include "mod_runtime.h"
#include <algorithm>

namespace {
bool enabled = false;
void reset() { enabled = false; }
void activate() { enabled = true; }
unsigned brightness(uint32_t pixel) {
    // Preserve the faithful red renderer's full brightness as well as the
    // color renderer's brightest channel. Output is monochrome per eye.
    return std::max({(pixel >> 16) & 255u, (pixel >> 8) & 255u, pixel & 255u});
}
}
VB_MOD_CONSTRUCTOR(register_stereo) {
    vb_mod_register_reset_callback(reset);
    vb_mod_register_activation_plugin("vb.display.anaglyph", activate);
}
extern "C" int vb_stereo_anaglyph_enabled() { return enabled; }
extern "C" void vb_stereo_anaglyph(uint32_t* left, const uint32_t* right, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const unsigned l = brightness(left[i]), r = brightness(right[i]);
        left[i] = 0xFF000000u | (l << 16) | (r << 8) | r;
    }
}
