/* Adapted from snesrecomp; PolyForm Noncommercial 1.0.0.
 * See ../licenses/snes-mod-runtime.txt. */
#pragma once

#include <stdint.h>

#ifdef __cplusplus
#include <filesystem>
#include <string>

#if defined(RECOMP_LAUNCHER)
#include "recomp_launcher.h"
#endif

namespace VBRecomp {

/*
 * Initialize the package catalog rooted at <exe>/mods for one verified game.
 * The ROM digest is the canonical lowercase SHA-256 used by package targets.
 */
bool mod_runtime_initialize(const std::filesystem::path& root,
                            const std::string& game_id,
                            const std::string& rom_sha256,
                            std::string* error = nullptr);

/*
 * Resolve the staged feature selections, validate the selected ROM, persist
 * state, and prepare the trusted-plugin activation plan.
 */
bool mod_runtime_commit(const std::filesystem::path& rom_path = {},
                        std::string* error = nullptr);

/* Invoke the trusted, statically linked plugins selected by the committed plan. */
void mod_runtime_activate_plugins();

#if defined(RECOMP_LAUNCHER)
const ::RecompLauncherCModProvider* mod_runtime_launcher_provider();
#endif

}  // namespace VBRecomp
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*VBModActivationCallback)(void);
typedef void (*VBModFrameCallback)(void);

struct RecompLauncherCModProvider;

int vb_mod_runtime_initialize_c(const char* root,
                                  const char* game_id,
                                  const char* rom_sha256);
int vb_mod_runtime_commit_c(const char* rom_path);
void vb_mod_runtime_activate_plugins_c(void);
const struct RecompLauncherCModProvider*
vb_mod_runtime_launcher_provider_c(void);
const char* vb_mod_runtime_last_error_c(void);

/*
 * Register a trusted implementation. A .vbmod archive may select only this
 * stable id; archives never provide native code, symbols, or library paths.
 */
int vb_mod_register_activation_plugin(const char* id,
                                        VBModActivationCallback callback);
/* Optional exclusive resource, e.g. "video.renderer". Conflicts are resolved
 * before activation, even when two features select different plugin IDs. */
int vb_mod_register_exclusive_plugin(const char* id, const char* resource,
                                      VBModActivationCallback callback);

/*
 * Register game-owned startup policy used to make a mod authoritative over a
 * legacy config flag. The reset callback runs before active plugins, so a
 * disabled feature reliably restores the stock behavior on every launch.
 */
int vb_mod_register_reset_callback(VBModActivationCallback callback);

/* Frame callbacks run at guest GAME_START, independent of host presentation. */
int vb_mod_register_frame_callback(VBModFrameCallback callback);
void vb_mod_runtime_frame_tick_c(void);

/* Headless and GUI hosts share the same package state. Mutations are staged;
 * commit verifies the original ROM and persists them before activation. */
int vb_mod_install_archive(const char* path);
int vb_mod_enable(const char* package, const char* feature, int enabled);
int vb_mod_set_option(const char* package, const char* feature,
                      const char* option, const char* value);
/* Activation callback scope only: copied values, never borrowed UI storage. */
int vb_mod_option(const char* option, char* out, unsigned capacity);
int vb_mod_asset(const char* relative_path, char* out, unsigned capacity);
/* Explicit owner-selected file/folder, validated and snapshotted at commit. */
int vb_mod_resource(const char* id, char* out, unsigned capacity);
void vb_mod_runtime_deactivate(void);

#if defined(_MSC_VER)
#pragma section(".CRT$XCU", read)
#define VB_MOD_CONSTRUCTOR(name)                                          \
    static void __cdecl name(void);                                         \
    __declspec(allocate(".CRT$XCU"))                                        \
    static void (__cdecl* name##_constructor)(void) = name;                 \
    static void __cdecl name(void)
#elif defined(__GNUC__) || defined(__clang__)
#define VB_MOD_CONSTRUCTOR(name)                                          \
    static void name(void) __attribute__((constructor));                    \
    static void name(void)
#else
#error "Virtual Boy mod plugin registration needs a supported constructor mechanism"
#endif

#ifdef __cplusplus
}
#endif
