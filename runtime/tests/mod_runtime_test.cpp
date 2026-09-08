#ifdef NDEBUG
#undef NDEBUG
#endif
#include "mod_runtime.h"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

static int activations, ticks, resets;
static void tick() { ++ticks; }
static void reset() { ++resets; }
static void activate() {
    ++activations;
    char value[64], path[1024];
    assert(vb_mod_option("strength", value, sizeof(value)));
    assert(!strcmp(value, "75"));
    assert(vb_mod_asset("palette.txt", path, sizeof(path)));
    assert(std::filesystem::is_regular_file(path));
    assert(!vb_mod_asset("../outside.txt", path, sizeof(path)));
    assert(vb_mod_register_frame_callback(tick));
}

int main(int argc, char** argv) {
    assert(argc == 2);
    namespace fs = std::filesystem;
    const fs::path root = argv[1];
    const auto mods = (root / "mods").string();
    const auto rom = (root / "rom.vb").string();
    const char* digest = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    assert(vb_mod_register_exclusive_plugin("test.color", "video.renderer", activate));
    assert(vb_mod_register_exclusive_plugin("test.alternate", "video.renderer", activate));
    assert(vb_mod_register_reset_callback(reset));
    assert(vb_mod_runtime_initialize_c(mods.c_str(), "test.game", digest));
    const int resets_before_commit = resets;
    vb_mod_runtime_activate_plugins_c();
    assert(resets == resets_before_commit); // Nothing may activate before verified commit.
    assert(!vb_mod_runtime_commit_c(nullptr));
    assert(!vb_mod_install_archive((root / "traversal.vbmod").string().c_str()));
    assert(!fs::exists(root / "escaped.txt"));
    for (const char* name : {"device", "ads", "trailing-dot", "absolute", "duplicate", "version-escape", "bad-crc"})
        assert(!vb_mod_install_archive((root / (std::string(name) + ".vbmod")).string().c_str()));
    assert(vb_mod_install_archive((root / "color.vbmod").string().c_str()));
    assert(!vb_mod_install_archive((root / "color.vbmod").string().c_str()));
    assert(!vb_mod_set_option("color", "color", "strength", "101"));
    assert(!vb_mod_set_option("color", "color", "strength", "9223372036854775808"));
    assert(vb_mod_set_option("color", "color", "strength", "75"));
    assert(vb_mod_runtime_commit_c(rom.c_str()));
    vb_mod_runtime_activate_plugins_c();
    assert(activations == 0); // Features default OFF.
    assert(vb_mod_enable("color", "color", 1));
    assert(!vb_mod_runtime_commit_c((root / "wrong.vb").string().c_str()));
    assert(vb_mod_runtime_commit_c(rom.c_str()));
    vb_mod_runtime_activate_plugins_c();
    assert(activations == 1);
    vb_mod_runtime_frame_tick_c();
    assert(ticks == 1);
    assert(vb_mod_install_archive((root / "conflict.vbmod").string().c_str()));
    assert(vb_mod_enable("conflict", "color", 1));
    assert(vb_mod_set_option("color", "color", "strength", "80"));
    assert(!vb_mod_runtime_commit_c(rom.c_str()));
    vb_mod_runtime_activate_plugins_c(); // Failed staged plan must still activate committed 75, not 80.
    assert(activations == 2);
    assert(vb_mod_set_option("color", "color", "strength", "75"));
    assert(vb_mod_enable("conflict", "color", 0));
    assert(vb_mod_install_archive((root / "wrong-target.vbmod").string().c_str()));
    assert(vb_mod_enable("wrong-target", "color", 1));
    assert(!vb_mod_runtime_commit_c(rom.c_str()));
    assert(vb_mod_enable("wrong-target", "color", 0));
    assert(vb_mod_runtime_commit_c(rom.c_str()));
    vb_mod_runtime_deactivate();
    vb_mod_runtime_frame_tick_c();
    assert(ticks == 1);
    // Reinitializing models process restart: saved feature/option selection survives.
    assert(vb_mod_runtime_initialize_c(mods.c_str(), "test.game", digest));
    assert(vb_mod_runtime_commit_c(rom.c_str()));
    vb_mod_runtime_activate_plugins_c();
    assert(activations == 3);
    assert(vb_mod_enable("color", "color", 0));
    assert(vb_mod_runtime_commit_c(rom.c_str()));
    vb_mod_runtime_activate_plugins_c();
    vb_mod_runtime_frame_tick_c();
    assert(activations == 3 && ticks == 1);
    std::ifstream file(rom, std::ios::binary);
    assert(std::string(std::istreambuf_iterator<char>(file), {}) == "abc");
#if defined(RECOMP_LAUNCHER)
    const auto* ui = vb_mod_runtime_launcher_provider_c();
    assert(ui && !strcmp(ui->archive_extension, ".vbmod"));
    assert(ui->package_count(ui->ctx) == 3 && ui->feature_count(ui->ctx) == 3);
    RecompLauncherCModFeature feature = {};
    assert(ui->feature_get(ui->ctx, 0, &feature));
    assert(!strcmp(feature.package_id, "color") && feature.enabled == 0);
    RecompLauncherCModOption option = {};
    assert(ui->feature_option_get(ui->ctx, "color", "color", 0, &option));
    assert(!strcmp(option.value, "75"));
    assert(!ui->remove_package(ui->ctx, "..", ".."));
    assert(fs::exists(rom));
    assert(ui->feature_enable(ui->ctx, "color", "color", 1));
    assert(!ui->remove_package(ui->ctx, "color", "1.0.0"));
    assert(ui->feature_enable(ui->ctx, "color", "color", 0));
    assert(ui->remove_package(ui->ctx, "color", "1.0.0"));
    assert(ui->package_count(ui->ctx) == 2);
#endif
}
