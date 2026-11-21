// rsdkv3 engine module: the rsdk-air ABI (include/rsdkair/engine.h) around RSDKv3
// (Sonic CD, 2011 decompilation).
//
// Paths: the game's data folder is read-only; settings.ini, SData.bin and mods/ live in
// its save folder. The engine finds Data.rsdk through settings.ini's [Dev] DataFile, set
// here to the data file's path relative to the save folder.
//
// No extra arguments yet: Sonic CD always starts with its own title screen and save
// select (a save-slot bridge like rsdkv4's is future work).
//
// SPDX-License-Identifier: MIT (this file); it links against the RSDKv3 decompilation.

#include "RetroEngine.hpp"

#include <rsdkair/engine.h>
#include <rsdkair/ini.hpp>
#include <rsdkair/paths.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

const char *const kRequired[] = {"Data.rsdk", nullptr};
const RsdkAirGameInfo kSonicCD = {"soniccd", "Sonic CD", kRequired};
const RsdkAirGameInfo *const kGames[] = {&kSonicCD, nullptr};

// Size of Engine.dataFile in RetroEngine.hpp, terminator included.
constexpr std::size_t kDataFileMax = 0x80;

std::string g_game_path;

bool supported(const char *id)
{
    for (const RsdkAirGameInfo *const *game = kGames; *game; ++game) {
        if (std::strcmp((*game)->id, id) == 0)
            return true;
    }
    return false;
}

} // namespace

// Where InitUserdata puts gamePath on macOS and Android (patched Userdata.cpp).
const char *rsdkair_rsdkv3_game_path() { return g_game_path.c_str(); }

extern "C" {

RSDKAIR_EXPORT int rsdkair_engine_abi_version(void) { return RSDKAIR_ABI_VERSION; }

RSDKAIR_EXPORT const RsdkAirGameInfo *const *rsdkair_engine_games(void) { return kGames; }

RSDKAIR_EXPORT int rsdkair_engine_main(const RsdkAirEngineArgs *args)
{
    if (!args || args->abi_version != RSDKAIR_ABI_VERSION || !args->game_id || !supported(args->game_id) ||
        !args->data_dir || !args->save_dir)
        return RSDKAIR_EXIT_BAD_ARGS;
    for (int i = 0; i < args->argc; ++i) {
        std::fprintf(stderr, "rsdkv3: unknown argument %s\n", args->argv[i]);
        return RSDKAIR_EXIT_BAD_ARGS;
    }

    const fs::path data_dir = rsdkair::path_from_utf8(args->data_dir);
    const fs::path save_dir = rsdkair::path_from_utf8(args->save_dir);

    // Data.rsdk relative to the save folder, which is where the engine resolves it from.
    // Absolute if no relative path exists (another drive).
    std::error_code ec;
    fs::path data_file = fs::relative(data_dir / "Data.rsdk", save_dir, ec);
    if (ec || data_file.empty())
        data_file = data_dir / "Data.rsdk";
    std::string data_file_text = rsdkair::path_to_utf8(data_file);
    for (char &c : data_file_text) {
        if (c == '\\')
            c = '/';
    }
    if (data_file_text.size() >= kDataFileMax) {
        std::fprintf(stderr, "rsdkv3: path to Data.rsdk is too long for the engine: %s\n", data_file_text.c_str());
        return RSDKAIR_EXIT_ENGINE_FAILED;
    }

    // settings.ini is the engine's own file; only the keys rsdk-air owns are set here.
    // DisableTouchControls only hides the on-screen d-pad (input is unchanged): with the
    // mobile Data.rsdk it shows up on desktop as soon as the mouse is used in a menu.
#if RETRO_PLATFORM == RETRO_ANDROID
    const char *hide_touch_pad = "false";
#else
    const char *hide_touch_pad = "true";
#endif
    const fs::path settings = save_dir / "settings.ini";
    if (!rsdkair::ini_set_file(settings, "Dev", "DataFile", data_file_text) ||
        !rsdkair::ini_set_file(settings, "Game", "DisableTouchControls", hide_touch_pad)) {
        std::fprintf(stderr, "rsdkv3: cannot write %s\n", rsdkair::path_to_utf8(settings).c_str());
        return RSDKAIR_EXIT_ENGINE_FAILED;
    }

#if RETRO_PLATFORM == RETRO_ANDROID
    g_game_path = rsdkair::path_to_utf8(save_dir) + "/";  // used as "<gamePath><file>"
#else
    g_game_path = rsdkair::path_to_utf8(save_dir);        // macOS: "<gamePath>/<file>"
#endif
    // Everywhere else the engine resolves files against the working directory (BASE_PATH
    // ""); on macOS the data file too.
    fs::current_path(save_dir, ec);
    if (ec) {
        std::fprintf(stderr, "rsdkv3: cannot enter %s\n", rsdkair::path_to_utf8(save_dir).c_str());
        return RSDKAIR_EXIT_ENGINE_FAILED;
    }

    Engine.Init();
    if (!Engine.running)
        return RSDKAIR_EXIT_ENGINE_FAILED;
    Engine.Run();
    return RSDKAIR_EXIT_OK;
}

} // extern "C"
