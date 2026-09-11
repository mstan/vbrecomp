#pragma once
#include "host.h"
#include "input.h"
#include <SDL.h>

inline constexpr uint16_t vb_host_pad_bits[14] = {
    VB_PAD_LUP, VB_PAD_LDOWN, VB_PAD_LLEFT, VB_PAD_LRIGHT,
    VB_PAD_RUP, VB_PAD_RDOWN, VB_PAD_RLEFT, VB_PAD_RRIGHT,
    VB_PAD_A, VB_PAD_B, VB_PAD_LT, VB_PAD_RT, VB_PAD_START, VB_PAD_SELECT
};

inline uint16_t vb_host_controller_pad(SDL_GameController* controller, const VbHostConfig& config) {
    if (!controller || config.player_source != 2) return 0;
    uint16_t pad = 0;
    const int threshold = config.deadzone * 32767 / 100;
    const auto axis_pressed = [&](int binding) {
        const int value = SDL_GameControllerGetAxis(controller,
            (SDL_GameControllerAxis)((binding - 100) / 2));
        return ((binding - 100) & 1) ? value > threshold : value < -threshold;
    };
    for (int i = 0; i < 14; ++i) {
        const int binding = config.pads[i];
        bool pressed = false;
        if (binding > 0 && binding < 100 && binding - 1 < SDL_CONTROLLER_BUTTON_MAX)
            pressed = SDL_GameControllerGetButton(controller, (SDL_GameControllerButton)(binding - 1)) != 0;
        else if (binding >= 100 && binding < 112)
            pressed = axis_pressed(binding);
        else if (binding >= 1000) {
            const unsigned mask = (unsigned)(binding - 1000);
            pressed = mask != 0;
            for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX && button < 16; ++button)
                if ((mask & (1u << button)) && !SDL_GameControllerGetButton(controller,
                    (SDL_GameControllerButton)button)) pressed = false;
        }
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
