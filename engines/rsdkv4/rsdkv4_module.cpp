// rsdkv4 engine module: the rsdk-air ABI (include/rsdkair/engine.h) around RSDKv4.
//
// Paths: the game's data folder is read-only; settings.ini, SData.bin and mods/ live in
// its save folder. The engine finds Data.rsdk through settings.ini's [Dev] DataFile, set
// here to the data file's path relative to the save folder.
//
//   extra arguments:  --slot <n>     start in save slot n (0-3), or -1 for no save
//                     --player <p>   character for a new game (0 Sonic, 1 Tails,
//                                    2 Knuckles, 3 Sonic & Tails)
//   Without --slot the game shows its own start menu.
//
// SPDX-License-Identifier: MIT (this file); it links against the RSDKv4 decompilation.

#include "RetroEngine.hpp"
#include "rsdkv4_bridge.hpp"

#include <rsdkair/engine.h>
#include <rsdkair/ini.hpp>
#include <rsdkair/paths.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>

namespace fs = std::filesystem;

namespace {

const char *const kRequired[] = {"Data.rsdk", nullptr};
const RsdkAirGameInfo kSonic1 = {"sonic1", "Sonic the Hedgehog", kRequired};
const RsdkAirGameInfo kSonic2 = {"sonic2", "Sonic the Hedgehog 2", kRequired};
const RsdkAirGameInfo *const kGames[] = {&kSonic1, &kSonic2, nullptr};

// Size of Engine.dataFile[0] in RetroEngine.hpp, terminator included.
constexpr std::size_t kDataFileMax = 0x80;

std::string g_game_path;

struct StartArgs {
    bool ok = true;
    bool has_slot = false;
    int slot = -1;
    int player = 0;
};

bool parse_int(const char *text, int &out)
{
    char *end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (!text[0] || *end)
        return false;
    out = static_cast<int>(value);
    return true;
}

StartArgs parse_args(int argc, char **argv)
{
    StartArgs args;
    for (int i = 0; i < argc; ++i) {
        const bool has_value = i + 1 < argc;
        if (std::strcmp(argv[i], "--slot") == 0 && has_value) {
            args.has_slot = parse_int(argv[++i], args.slot) && args.slot >= -1 && args.slot <= 3;
            args.ok = args.ok && args.has_slot;
        } else if (std::strcmp(argv[i], "--player") == 0 && has_value) {
            args.ok = args.ok && parse_int(argv[++i], args.player) && args.player >= 0 && args.player <= 3;
        } else {
            std::fprintf(stderr, "rsdkv4: unknown argument %s\n", argv[i]);
            args.ok = false;
        }
    }
    return args;
}

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
const char *rsdkair_rsdkv4_game_path() { return g_game_path.c_str(); }

extern "C" {

RSDKAIR_EXPORT int rsdkair_engine_abi_version(void) { return RSDKAIR_ABI_VERSION; }

RSDKAIR_EXPORT const RsdkAirGameInfo *const *rsdkair_engine_games(void) { return kGames; }

RSDKAIR_EXPORT int rsdkair_engine_main(const RsdkAirEngineArgs *args)
{
    if (!args || args->abi_version != RSDKAIR_ABI_VERSION || !args->game_id || !supported(args->game_id) ||
        !args->data_dir || !args->save_dir)
        return RSDKAIR_EXIT_BAD_ARGS;

    const StartArgs start = parse_args(args->argc, args->argv);
    if (!start.ok)
        return RSDKAIR_EXIT_BAD_ARGS;

    const fs::path data_dir = rsdkair::path_from_utf8(args->data_dir);
    const fs::path save_dir = rsdkair::path_from_utf8(args->save_dir);

    // Data.rsdk relative to the save folder, which is where the engine resolves it from
    // on every platform below. Absolute if no relative path exists (another drive).
    std::error_code ec;
    fs::path data_file = fs::relative(data_dir / "Data.rsdk", save_dir, ec);
    if (ec || data_file.empty())
        data_file = data_dir / "Data.rsdk";
    std::string data_file_text = rsdkair::path_to_utf8(data_file);  // UTF-8; this file is C++17
    for (char &c : data_file_text) {
        if (c == '\\')
            c = '/';
    }
    if (data_file_text.size() >= kDataFileMax) {
        std::fprintf(stderr, "rsdkv4: path to Data.rsdk is too long for the engine: %s\n", data_file_text.c_str());
        return RSDKAIR_EXIT_ENGINE_FAILED;
    }

    // settings.ini is the engine's own file; only the keys rsdk-air owns are set here.
    const fs::path settings = save_dir / "settings.ini";
    if (!rsdkair::ini_set_file(settings, "Dev", "DataFile", data_file_text) ||
        !rsdkair::ini_set_file(settings, "Game", "SkipStartMenu", start.has_slot ? "true" : "false")) {
        std::fprintf(stderr, "rsdkv4: cannot write %s\n", rsdkair::path_to_utf8(settings).c_str());
        return RSDKAIR_EXIT_ENGINE_FAILED;
    }

#if RETRO_PLATFORM == RETRO_ANDROID
    g_game_path = rsdkair::path_to_utf8(save_dir) + "/";  // used as "<gamePath><file>"
#elif RETRO_PLATFORM == RETRO_OSX
    g_game_path = rsdkair::path_to_utf8(save_dir);        // used as "<gamePath>/<file>"
#else
    // Windows and Linux resolve everything against the working directory (BASE_PATH "").
    fs::current_path(save_dir, ec);
    if (ec) {
        std::fprintf(stderr, "rsdkv4: cannot enter %s\n", rsdkair::path_to_utf8(save_dir).c_str());
        return RSDKAIR_EXIT_ENGINE_FAILED;
    }
#endif

    Engine.Init();
    if (!Engine.running)
        return RSDKAIR_EXIT_ENGINE_FAILED;

    if (start.has_slot)
        rsdkv4_bridge::start_game(start.slot, start.player);

    Engine.Run();
    return RSDKAIR_EXIT_OK;
}

} // extern "C"
