#include "host.h"
#include "mod_runtime.h"
#include "renderer.h"
#include "memory.h"
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <vector>
#include <cstring>
#if VB_RUNTIME_HAVE_SDL
#include <SDL.h>
#endif
#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#include "launcher_profile.h"
#endif
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
#ifndef VB_GAME_ID
#define VB_GAME_ID ""
#define VB_GAME_SHA256 ""
#define VB_GAME_TITLE "Virtual Boy Recompiled"
#define VB_GAME_CRC32 0
#endif

VbHostConfig vb_host_config;
namespace {
namespace fs = std::filesystem;
fs::path config_path, mods_path;
fs::path sram_path;
bool sram_disabled=false;
std::string last_rom, error;
bool no_launcher = false, force_launcher = false;
struct Action { std::string kind, value; };
std::vector<Action> actions;

void load_config() {
    std::ifstream file(config_path);
    std::string key;
    while (file >> key) {
        if (key == "rom") { file >> std::quoted(last_rom); continue; }
        if (key == "gamepad") { file >> std::quoted(vb_host_config.gamepad_guid); continue; }
        int value;
        if (!(file >> value)) break;
        if (key == "scale") vb_host_config.scale = std::clamp(value, 1, 6);
        else if (key == "fullscreen") vb_host_config.fullscreen = std::clamp(value, 0, 2);
        else if (key == "filter") vb_host_config.filter = value != 0;
        else if (key == "audio") vb_host_config.audio = value != 0;
        else if (key == "volume") vb_host_config.volume = std::clamp(value, 0, 100);
        else if (key == "source") vb_host_config.player_source = std::clamp(value, 0, 2);
        else if (key == "deadzone") vb_host_config.deadzone = std::clamp(value, 0, 100);
        else if (key == "skip_launcher") vb_host_config.skip_launcher = value != 0;
        for (int i = 0; i < 14; ++i) {
            if (key == "key" + std::to_string(i)) vb_host_config.keys[i] = std::clamp(value, 0, 511);
            if (key == "pad" + std::to_string(i)) vb_host_config.pads[i] = std::clamp(value, 0, 65535);
        }
    }
}
}

int vb_host_argument(int& i, int argc, char** argv) {
    const std::string arg = argv[i];
    if (arg == "--no-launcher") { no_launcher = true; return 1; }
    if (arg == "--launcher") { no_launcher = false; force_launcher = true; return 1; }
    if (arg == "--no-save") { sram_disabled = true; return 1; }
    if (arg != "--mods-dir" && arg != "--config" && arg != "--install-mod" &&
        arg != "--enable-mod" && arg != "--disable-mod" && arg != "--save") return 0;
    if (i + 1 == argc) { error = "Missing value for " + arg; return -1; }
    const std::string value = argv[++i];
    if (arg == "--mods-dir") mods_path = value;
    else if (arg == "--config") config_path = value;
    else if (arg == "--save") sram_path = value;
    else actions.push_back({arg, value});
    return 1;
}

void vb_host_save(void*) {
    error.clear();
    if (config_path.empty()) return;
    std::error_code ec;
    fs::create_directories(config_path.parent_path(), ec);
    const auto temporary = fs::path(config_path.string() + ".tmp");
    std::ofstream file(temporary, std::ios::trunc);
    if (!file) { error = "Cannot save settings."; return; }
    file << "rom " << std::quoted(last_rom) << '\n'
         << "scale " << vb_host_config.scale << '\n'
         << "fullscreen " << vb_host_config.fullscreen << '\n'
         << "filter " << vb_host_config.filter << '\n'
         << "audio " << vb_host_config.audio << '\n'
         << "volume " << vb_host_config.volume << '\n'
         << "source " << vb_host_config.player_source << '\n'
         << "deadzone " << vb_host_config.deadzone << '\n'
         << "skip_launcher " << vb_host_config.skip_launcher << '\n'
         << "gamepad " << std::quoted(vb_host_config.gamepad_guid) << '\n';
    for (int i = 0; i < 14; ++i)
        file << "key" << i << ' ' << vb_host_config.keys[i] << '\n'
             << "pad" << i << ' ' << vb_host_config.pads[i] << '\n';
    file.close();
    if (!file) { error = "Cannot finish saving settings."; return; }
#ifdef _WIN32
    if (!MoveFileExW(temporary.c_str(), config_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        error = "Cannot publish saved settings.";
#else
    fs::rename(temporary, config_path, ec);
    if (ec) error = "Cannot publish saved settings: " + ec.message();
#endif
}

const std::string& vb_host_error() { return error; }

int vb_host_prepare(const char* executable, const char*& rom, bool headless) {
    vb_mod_register_reset_callback(vb_renderer_reset);
    fs::path base = fs::absolute(executable).parent_path();
#if VB_RUNTIME_HAVE_SDL
    if (char* path = SDL_GetBasePath()) { base = path; SDL_free(path); }
#endif
    if (config_path.empty()) config_path = base / "vbrecomp.cfg";
    config_path = fs::absolute(config_path);
    if (sram_path.empty()) {
        if (headless) sram_disabled=true;
        sram_path=config_path.parent_path()/"saves"/(std::string(VB_GAME_ID)+".sav");
    }
    sram_path=fs::absolute(sram_path);
    if (mods_path.empty()) mods_path = base / "mods";
    load_config();
    if (rom) last_rom = rom;
    const bool has_game = VB_GAME_ID[0] != 0;
    if (!has_game && !actions.empty()) {
        error = "This executable has no game identity for mod packages."; return -1;
    }
    if (has_game && !vb_mod_runtime_initialize_c(mods_path.string().c_str(), VB_GAME_ID, VB_GAME_SHA256)) {
        error = vb_mod_runtime_last_error_c(); return -1;
    }
    for (const auto& action : actions) {
        int ok = 0;
        if (action.kind == "--install-mod") ok = vb_mod_install_archive(action.value.c_str());
        else {
            const auto colon = action.value.find(':');
            if (colon != std::string::npos)
                ok = vb_mod_enable(action.value.substr(0, colon).c_str(), action.value.substr(colon + 1).c_str(),
                                   action.kind == "--enable-mod");
        }
        if (!ok) { error = "Mod action failed: " + std::string(vb_mod_runtime_last_error_c()); return -1; }
    }
#if defined(RECOMP_LAUNCHER)
    if (!headless && !no_launcher && (force_launcher || !vb_host_config.skip_launcher || last_rom.empty()) && has_game) {
        RecompLauncherCSettings settings = {};
        settings.window_scale = vb_host_config.scale;
        settings.fullscreen = vb_host_config.fullscreen;
        settings.linear_filter = vb_host_config.filter;
        settings.enable_audio = vb_host_config.audio;
        settings.volume = vb_host_config.volume;
        settings.player_src[0] = vb_host_config.player_source;
        settings.deadzone[0] = vb_host_config.deadzone;
        settings.skip_launcher = vb_host_config.skip_launcher;
        std::strncpy(settings.player_gamepad_guid[0], vb_host_config.gamepad_guid.c_str(), 39);
        std::copy_n(vb_host_config.keys, 14, settings.player_key_bind[0]);
        std::copy_n(vb_host_config.pads, 14, settings.player_pad_bind[0]);
        RecompLauncherCGameInfo game = {};
        launcher_profile_apply("vb", &game);
        game.name = VB_GAME_TITLE;
        game.region = "Japan / USA";
        game.expected_crc = VB_GAME_CRC32;
        game.has_expected_crc = VB_GAME_CRC32 != 0;
        game.mods = vb_mod_runtime_launcher_provider_c();
        const std::string config_text = config_path.string();
        game.config_path = config_text.c_str();
        RecompLauncherCSettings defaults = settings;
        const VbHostConfig default_values;
        defaults.window_scale = 2;
        defaults.fullscreen = defaults.linear_filter = defaults.skip_launcher = 0;
        defaults.enable_audio = 1;
        defaults.volume = 100;
        defaults.player_src[0] = 1;
        defaults.deadzone[0] = 35;
        defaults.player_gamepad_guid[0][0] = 0;
        std::copy_n(default_values.keys, 14, defaults.player_key_bind[0]);
        std::copy_n(default_values.pads, 14, defaults.player_pad_bind[0]);
        game.default_settings = &defaults;
        char selected[4096] = {};
        const int result = recomp_launcher_run_window(VB_GAME_TITLE, &settings, &game,
            (base / "assets").string().c_str(), last_rom.c_str(), selected, sizeof(selected));
        if (result == RECOMP_LAUNCHER_RESULT_QUIT) return 0;
        if (result != RECOMP_LAUNCHER_RESULT_LAUNCH) { error = "The launcher could not start."; return -1; }
        last_rom = selected;
        vb_host_config.scale = std::clamp(settings.window_scale, 1, 6);
        vb_host_config.fullscreen = std::clamp(settings.fullscreen, 0, 2);
        vb_host_config.filter = settings.linear_filter != 0;
        vb_host_config.audio = settings.enable_audio != 0;
        vb_host_config.volume = std::clamp(settings.volume, 0, 100);
        vb_host_config.player_source = settings.player_src[0];
        vb_host_config.deadzone = std::clamp(settings.deadzone[0], 0, 100);
        vb_host_config.skip_launcher = settings.skip_launcher != 0;
        vb_host_config.gamepad_guid = settings.player_gamepad_guid[0];
        std::copy_n(settings.player_key_bind[0], 14, vb_host_config.keys);
        std::copy_n(settings.player_pad_bind[0], 14, vb_host_config.pads);
    }
#else
    (void)headless;
#endif
    rom = last_rom.empty() ? nullptr : last_rom.c_str();
    if (has_game && !rom) { error = "Select the original game ROM or pass --rom PATH."; return -1; }
    if (has_game && rom) {
        if (!vb_mod_runtime_commit_c(rom)) { error = vb_mod_runtime_last_error_c(); return -1; }
        vb_mod_runtime_activate_plugins_c();
        vb_host_save();
        if (!error.empty()) return -1;
    }
    return 1;
}

bool vb_host_commit_mods() {
    if (!vb_mod_runtime_commit_c(last_rom.c_str())) { error = vb_mod_runtime_last_error_c(); return false; }
    vb_mod_runtime_activate_plugins_c();
    return true;
}

bool vb_host_load_sram() {
    if(sram_disabled) return true;
    std::error_code ec;
    if(!fs::exists(sram_path,ec)) { if(ec) { error=ec.message();return false; } return true; }
    if(fs::file_size(sram_path,ec)!=65536 || ec) { error="Cartridge save must be exactly 65536 bytes: "+sram_path.string();return false; }
    std::ifstream file(sram_path,std::ios::binary);
    file.read((char*)vb_cart_ram_data(),65536);
    if(!file) { error="Cannot read cartridge save: "+sram_path.string();return false; }
    return true;
}
bool vb_host_save_sram() {
    if(sram_disabled) return true;
    std::error_code ec;fs::create_directories(sram_path.parent_path(),ec);
    if(ec) { error=ec.message();return false; }
    auto temporary=fs::path(sram_path.string()+".tmp");
    std::ofstream file(temporary,std::ios::binary|std::ios::trunc);
    file.write((const char*)vb_cart_ram_data(),65536);file.close();
    if(!file) { error="Cannot write cartridge save: "+temporary.string();return false; }
#ifdef _WIN32
    if(!MoveFileExW(temporary.c_str(),sram_path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {
        error="Cannot publish cartridge save: "+sram_path.string();return false;
    }
#else
    fs::rename(temporary,sram_path,ec);
    if(ec) { error=ec.message();return false; }
#endif
    return true;
}
