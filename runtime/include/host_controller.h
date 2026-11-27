#pragma once
#include "host.h"
#include "input.h"
#include <SDL.h>
#include "recomp_input.h"

inline constexpr uint16_t vb_host_pad_bits[14] = {
    VB_PAD_LUP, VB_PAD_LDOWN, VB_PAD_LLEFT, VB_PAD_LRIGHT,
    VB_PAD_RUP, VB_PAD_RDOWN, VB_PAD_RLEFT, VB_PAD_RRIGHT,
    VB_PAD_A, VB_PAD_B, VB_PAD_LT, VB_PAD_RT, VB_PAD_START, VB_PAD_SELECT
};

inline uint16_t vb_host_controller_pad(SDL_GameController* controller, const VbHostPlayer& config) {
    if (!controller || config.player_source != 2) return 0;
    uint16_t pad = 0;
    const int threshold = config.deadzone * 32767 / 100;
    ri_device device{};
    for(int button=0;button<SDL_CONTROLLER_BUTTON_MAX && button<32;++button)
        if(SDL_GameControllerGetButton(controller,(SDL_GameControllerButton)button))device.buttons|=1u<<button;
    for(int axis=0;axis<RI_MAX_AXES;++axis)device.axes[axis]=SDL_GameControllerGetAxis(controller,(SDL_GameControllerAxis)axis);
    const auto axis_pressed=[&](int binding){return ri_binding_held(&device,(uint32_t)binding,threshold)!=0;};
    for (int i = 0; i < 14; ++i) {
        const int binding = config.pads[i];
        bool pressed=ri_binding_held(&device,(uint32_t)binding,threshold)!=0;
        // The default D-pad also accepts the left stick. Per-axis thresholds
        // avoid accidental vertical input from small drift during steering.
        // Rebound or explicitly unbound directions keep exactly their binding.
        if (i < 4 && binding == 12 + i) {
            constexpr int stick_bindings[] = {102, 103, 100, 101};
            pressed = pressed || axis_pressed(stick_bindings[i]);
        }
        if (pressed) pad |= vb_host_pad_bits[i];
    }
    return pad;
}
