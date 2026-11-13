#include "stereo.h"
#include "mod_runtime.h"
#include <array>
#include <cassert>
#include <cstring>

static VBModActivationCallback reset_cb, activate_cb;
extern "C" int vb_mod_register_reset_callback(VBModActivationCallback cb) { reset_cb=cb; return 1; }
extern "C" int vb_mod_register_activation_plugin(const char* id, VBModActivationCallback cb) {
    assert(!std::strcmp(id,"vb.display.anaglyph")); activate_cb=cb; return 1;
}
int main() {
    assert(reset_cb && activate_cb && !vb_stereo_anaglyph_enabled());
    activate_cb(); assert(vb_stereo_anaglyph_enabled());
    // Different spatial positions in the real eye images must remain distinct:
    // copying the left eye twice would turn the first red pixel white.
    std::array<uint32_t,5> left{0xFFFF0000,0xFF000000,0xFF001F00,0xFF000000,0xFFFFFFFF};
    const std::array<uint32_t,5> right{0xFF000000,0xFFFF0000,0xFF00007F,0xFF000000,0xFFFFFFFF};
    const auto original_right=right;
    vb_stereo_anaglyph(left.data(),right.data(),left.size());
    assert((left==std::array<uint32_t,5>{0xFFFF0000,0xFF00FFFF,0xFF1F7F7F,0xFF000000,0xFFFFFFFF}));
    assert(right==original_right);
    reset_cb(); assert(!vb_stereo_anaglyph_enabled());
}
