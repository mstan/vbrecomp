#include "host_controllers.h"
#include <cassert>
#include <set>
VbHostConfig vb_host_config;
unsigned vb_host_player_count(){return 4;}
VbHostPlayer& vb_host_player(unsigned seat){return seat?vb_host_config.extra_players[seat-1]:vb_host_config;}
int main() {
    SDL_SetMainReady();assert(SDL_Init(SDL_INIT_GAMECONTROLLER)==0);
    std::array<SDL_Joystick*,4> virtuals{};
    for(unsigned i=0;i<4;++i) {
        const int index=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,15,0);
        assert(index>=0 && SDL_IsGameController(index));virtuals[i]=SDL_JoystickOpen(index);assert(virtuals[i]);
    }
    VbHostControllers controllers;controllers.refresh();
    // A keyboard player cannot consume the controller needed by player two.
    assert(!controllers.pads[0]);
    assert(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controllers.pads[1]))==SDL_JoystickInstanceID(virtuals[0]));
    controllers.close();vb_host_config.player_source=2;controllers.refresh();
    std::set<SDL_JoystickID> ids;
    for(unsigned i=0;i<4;++i) {
        assert(controllers.pads[i]);ids.insert(SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(controllers.pads[i])));
        assert(SDL_JoystickSetVirtualButton(virtuals[i],SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,i==2)==0);
    }
    assert(ids.size()==4);SDL_JoystickUpdate();SDL_GameControllerUpdate();
    for(unsigned i=0;i<4;++i)assert(bool(controllers.read(i)&VB_PAD_RT)==(i==2));
    const auto removed=SDL_JoystickInstanceID(virtuals[1]);
    SDL_JoystickClose(virtuals[1]);virtuals[1]=nullptr;assert(SDL_JoystickDetachVirtual(1)==0);
    controllers.removed(removed);assert(!controllers.pads[1] && controllers.read(1)==0);
    assert(controllers.pads[0] && controllers.pads[2] && controllers.pads[3]);
    const int replacement=SDL_JoystickAttachVirtual(SDL_JOYSTICK_TYPE_GAMECONTROLLER,6,15,0);assert(replacement>=0);
    controllers.refresh();assert(controllers.pads[1] && controllers.read(1)==0);
    controllers.close();for(auto* stick:virtuals)if(stick)SDL_JoystickClose(stick);
    SDL_Quit();
}
