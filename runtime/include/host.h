#pragma once
#include <string>

struct VbHostConfig {
    int scale = 2, fullscreen = 0, filter = 0, audio = 1, volume = 100;
    int player_source = 1, deadzone = 35, skip_launcher = 0;
    std::string gamepad_guid;
    int keys[14] = {82,81,80,79,26,22,4,7,27,29,20,8,40,229};
    int pads[14] = {12,13,14,15,106,107,104,105,1,2,10,11,7,5};
};
extern VbHostConfig vb_host_config;
/* Return 1 if consumed, 0 if not a host argument, -1 for missing/invalid value. */
int vb_host_argument(int& index, int argc, char** argv);
/* 1 play/idle, 0 user quit, -1 error. ROM pointer then refers to host-owned storage. */
int vb_host_prepare(const char* executable, const char*& rom, bool headless);
void vb_host_save(void* unused = nullptr);
const std::string& vb_host_error();
bool vb_host_commit_mods();
