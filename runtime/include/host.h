#pragma once
#include <string>

struct VbHostPlayer {
    int player_source=1,deadzone=35;
    std::string gamepad_guid;
    int keys[14] = {82,81,80,79,26,22,4,7,27,29,20,8,40,229};
    int pads[14] = {12,13,14,15,106,107,104,105,1,2,10,11,7,5};
};
struct VbHostConfig:VbHostPlayer {
    int scale = 2, fullscreen = 0, filter = 0, audio = 1, volume = 100;
    int skip_launcher = 0;
    int rewind = 1, rewind_depth = 32, rewind_interval = 6;
    VbHostPlayer extra_players[3];
    VbHostConfig(){for(auto& p:extra_players)p.player_source=2;}
};
extern VbHostConfig vb_host_config;
unsigned vb_host_player_count();
VbHostPlayer& vb_host_player(unsigned seat);
/* Return 1 if consumed, 0 if not a host argument, -1 for missing/invalid value. */
int vb_host_argument(int& index, int argc, char** argv);
/* 1 play/idle, 0 user quit, -1 error. ROM pointer then refers to host-owned storage. */
int vb_host_prepare(const char* executable, const char*& rom, bool headless);
int vb_host_return_to_launcher(const char* executable,const char*& rom,bool headless);
void vb_host_save(void* unused = nullptr);
const std::string& vb_host_error();
bool vb_host_commit_mods();
bool vb_host_load_sram();
bool vb_host_save_sram();
/* Session sandbox: disable native-save loading/publishing for this process.
 * At startup this leaves the user's persistent progress entirely untouched. */
void vb_host_sandbox_sram();
/* Reversible local multiplayer mode. Preserve the live single-player native
 * save before entering; restore it on a cold return to the title. A loaded
 * multiplayer snapshot supplies the pre-restore SRAM instead of its own
 * candidate bytes. --no-save remains permanent; network sessions separately
 * suppress all native-save IO until a new offline launch is selected. */
bool vb_host_begin_multiplayer_sram(const unsigned char* previous=nullptr,bool fresh=false);
bool vb_host_end_multiplayer_sram(bool restore_baseline);
