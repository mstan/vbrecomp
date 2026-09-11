#include "host_controller.h"
#include <cassert>

int main() {
    SDL_SetMainReady();
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    assert(SDL_Init(SDL_INIT_GAMECONTROLLER) == 0);
    const int device = SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,
        SDL_CONTROLLER_AXIS_MAX, SDL_CONTROLLER_BUTTON_MAX, 0);
    assert(device >= 0);
    SDL_GameController* controller = SDL_GameControllerOpen(device);
    assert(controller);
    SDL_Joystick* joystick = SDL_GameControllerGetJoystick(controller);
    VbHostConfig config;
    config.player_source = 2;
    const auto read = [&] {
        SDL_GameControllerUpdate();
        return vb_host_controller_pad(controller, config);
    };
    const auto axis = [&](SDL_GameControllerAxis which, Sint16 value) {
        assert(SDL_JoystickSetVirtualAxis(joystick, which, value) == 0);
    };
    const struct { SDL_GameControllerButton button; uint16_t expected; } buttons[] = {
        {SDL_CONTROLLER_BUTTON_DPAD_UP, VB_PAD_LUP},
        {SDL_CONTROLLER_BUTTON_DPAD_DOWN, VB_PAD_LDOWN},
        {SDL_CONTROLLER_BUTTON_DPAD_LEFT, VB_PAD_LLEFT},
        {SDL_CONTROLLER_BUTTON_DPAD_RIGHT, VB_PAD_LRIGHT},
        {SDL_CONTROLLER_BUTTON_A, VB_PAD_A}, {SDL_CONTROLLER_BUTTON_B, VB_PAD_B},
        {SDL_CONTROLLER_BUTTON_LEFTSHOULDER, VB_PAD_LT},
        {SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, VB_PAD_RT},
        {SDL_CONTROLLER_BUTTON_START, VB_PAD_START}, {SDL_CONTROLLER_BUTTON_BACK, VB_PAD_SELECT}
    };
    assert(read() == 0);
    for (const auto& test : buttons) {
        assert(SDL_JoystickSetVirtualButton(joystick, test.button, 1) == 0);
        assert(read() == test.expected);
        assert(SDL_JoystickSetVirtualButton(joystick, test.button, 0) == 0);
        assert(read() == 0);
    }
    axis(SDL_CONTROLLER_AXIS_LEFTX, 24000);
    axis(SDL_CONTROLLER_AXIS_LEFTY, 500); // Steering must not pick up small vertical drift.
    assert(read() == VB_PAD_LRIGHT);
    axis(SDL_CONTROLLER_AXIS_LEFTY, -24000);
    assert(read() == (VB_PAD_LUP | VB_PAD_LRIGHT));
    config.pads[3] = 0; // Unbound Right must not regain a stick alias.
    assert(read() == VB_PAD_LUP);
    config.pads[0] = 4; // Rebound Up now uses Y only.
    assert(read() == 0);
    assert(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_Y, 1) == 0);
    assert(read() == VB_PAD_LUP);
    assert(SDL_JoystickSetVirtualButton(joystick, SDL_CONTROLLER_BUTTON_Y, 0) == 0);
    config = VbHostConfig{};
    config.player_source = 2;
    axis(SDL_CONTROLLER_AXIS_LEFTX, 0);
    axis(SDL_CONTROLLER_AXIS_LEFTY, 0);
    axis(SDL_CONTROLLER_AXIS_RIGHTX, -24000);
    axis(SDL_CONTROLLER_AXIS_RIGHTY, 24000);
    assert(read() == (VB_PAD_RLEFT | VB_PAD_RDOWN));
    config.player_source = 1;
    assert(read() == 0); // Keyboard selection excludes the controller.
    config.player_source = 0;
    assert(read() == 0);
    SDL_GameControllerClose(controller);
    assert(SDL_JoystickDetachVirtual(device) == 0);
    SDL_Quit();
}
