// rsdkv5 engine module: the rsdk-air ABI (include/rsdkair/engine.h) around RSDKv5U with
// Sonic Mania's game logic linked in (no Game.dll / libGame.so next to the engine).
//
// Paths: the game's data folder is read-only; Settings.ini, the save file and mods/ live
// in its save folder, the engine's user file directory. The engine finds Data.rsdk through
// Settings.ini's [Game] dataFile, set here to the data file's path relative to the save
// folder.
//
// No extra arguments yet: Mania starts with its own title screen and menus.
//
// SPDX-License-Identifier: MIT (this file); it links against the RSDKv5 and Sonic Mania
// decompilations.

#include "RSDK/Core/RetroEngine.hpp"

#define EngineInfo RSDK::EngineInfo
#include <GameMain.h>
#undef EngineInfo

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
const RsdkAirGameInfo kMania = {"mania", "Sonic Mania", kRequired};
const RsdkAirGameInfo *const kGames[] = {&kMania, nullptr};

// RSDK::SKU::userFileDir is 0x100 bytes and the data pack path is built from it.
constexpr std::size_t kDataFileMax = 0x80;

std::string g_user_dir;

bool supported(const char *id)
{
    for (const RsdkAirGameInfo *const *game = kGames; *game; ++game) {
        if (std::strcmp((*game)->id, id) == 0)
            return true;
    }
    return false;
}

} // namespace

// Where InitUserDirectory points the engine on macOS (patched UserStorage.cpp).
const char *rsdkair_rsdkv5_user_dir() { return g_user_dir.c_str(); }

extern "C" {

RSDKAIR_EXPORT int rsdkair_engine_abi_version(void) { return RSDKAIR_ABI_VERSION; }

RSDKAIR_EXPORT const RsdkAirGameInfo *const *rsdkair_engine_games(void) { return kGames; }

RSDKAIR_EXPORT int rsdkair_engine_main(const RsdkAirEngineArgs *args)
{
    if (!args || args->abi_version != RSDKAIR_ABI_VERSION || !args->game_id || !supported(args->game_id) ||
        !args->data_dir || !args->save_dir)
        return RSDKAIR_EXIT_BAD_ARGS;
    for (int i = 0; i < args->argc; ++i) {
        std::fprintf(stderr, "rsdkv5: unknown argument %s\n", args->argv[i]);
        return RSDKAIR_EXIT_BAD_ARGS;
    }

    const fs::path data_dir = rsdkair::path_from_utf8(args->data_dir);
    const fs::path save_dir = rsdkair::path_from_utf8(args->save_dir);

    // Data.rsdk relative to the save folder (the engine prefixes the user file directory).
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
        std::fprintf(stderr, "rsdkv5: path to Data.rsdk is too long for the engine: %s\n", data_file_text.c_str());
        return RSDKAIR_EXIT_ENGINE_FAILED;
    }

    // Settings.ini is the engine's own file; only the key rsdk-air owns is set here. When
    // the file is new, the engine fills in its defaults for every other key.
    const fs::path settings = save_dir / "Settings.ini";
    if (!rsdkair::ini_set_file(settings, "Game", "dataFile", data_file_text)) {
        std::fprintf(stderr, "rsdkv5: cannot write %s\n", rsdkair::path_to_utf8(settings).c_str());
        return RSDKAIR_EXIT_ENGINE_FAILED;
    }

    // Linux ("./") and Windows ("") resolve the user file directory against the working
    // directory; macOS gets the folder itself.
    g_user_dir = rsdkair::path_to_utf8(save_dir) + "/";
    fs::current_path(save_dir, ec);
    if (ec) {
        std::fprintf(stderr, "rsdkv5: cannot enter %s\n", rsdkair::path_to_utf8(save_dir).c_str());
        return RSDKAIR_EXIT_ENGINE_FAILED;
    }

    // What upstream's RSDK_main (main.cpp) does, with the game logic linked statically.
    RSDK::linkGameLogic = reinterpret_cast<RSDK::LogicLinkHandle>(&LinkGameLogicDLL);
    RSDK::InitCoreAPI();
    const int code = RSDK::RunRetroEngine(0, nullptr);
    RSDK::ReleaseCoreAPI();
    return code == 0 ? RSDKAIR_EXIT_OK : RSDKAIR_EXIT_ENGINE_FAILED;
}

} // extern "C"
