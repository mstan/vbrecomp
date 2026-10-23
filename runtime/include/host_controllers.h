#pragma once
#include "host_controller.h"
#include <array>
// One device instance belongs to one configured seat. Matching GUIDs may be
// identical on two physical pads; instance IDs keep those seats independent.
class VbHostControllers {
public:
    std::array<SDL_GameController*,4> pads{};
    void refresh() {
        for(unsigned seat=0;seat<vb_host_player_count();++seat) {
            if(pads[seat] || vb_host_player(seat).player_source!=2)continue;
            for(int i=0;i<SDL_NumJoysticks();++i) {
                if(!SDL_IsGameController(i))continue;
                const auto id=SDL_JoystickGetDeviceInstanceID(i);bool claimed=false;
                for(auto* pad:pads)if(pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))==id)claimed=true;
                if(claimed)continue;
                char guid[40]{};SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(i),guid,sizeof(guid));
                const auto& player=vb_host_player(seat);
                if(!player.gamepad_guid.empty() && player.gamepad_guid!=guid)continue;
                pads[seat]=SDL_GameControllerOpen(i);if(pads[seat])break;
            }
        }
    }
    void removed(SDL_JoystickID id) {
        for(auto*& pad:pads)if(pad && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad))==id) {
            SDL_GameControllerClose(pad);pad=nullptr;
        }
        refresh();
    }
    uint16_t read(unsigned seat) const {
        return seat<pads.size()?vb_host_controller_pad(pads[seat],vb_host_player(seat)):0;
    }
    void close(){for(auto*& pad:pads)if(pad){SDL_GameControllerClose(pad);pad=nullptr;}}
};
