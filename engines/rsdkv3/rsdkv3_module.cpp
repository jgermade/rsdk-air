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
#include "menu_glue.hpp"

#include <rsdkair/engine.h>
#include <rsdkair/ini.hpp>
#include <rsdkair/paths.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

extern SDL_AudioDeviceID audioDevice;  // RSDKv3's Audio.cpp (not in its headers)

namespace {

const char *const kRequired[] = {"Data.rsdk", nullptr};
const RsdkAirGameInfo kSonicCD = {"soniccd", "Sonic CD", kRequired};
const RsdkAirGameInfo *const kGames[] = {&kSonicCD, nullptr};

// ---- In-game menu (engines/common/menu_glue.hpp) ---------------------------------------
// No save slots here (no slot bridge for Sonic CD yet): Play, the game's own screens,
// settings, back to the launcher.

rsdkair_menu::Glue g_menu;

struct Screen {
    int list;
    int stage;
    const char *label;
};
std::vector<Screen> g_screens;
bool g_screens_found = false;

void find_screens()
{
    if (g_screens_found)
        return;
    g_screens_found = true;
    static const struct {
        const char *name;
        const char *label;
    } kWanted[] = {{"TITLE SCREEN", "Title screen"},
                   {"LEVEL SELECT", "Level select"},
                   {"STAGE SELECT", "Stage select"},
                   {"STAGE MENU", "Stage menu"}};
    for (const auto &wanted : kWanted) {
        bool found = false;
        for (int list = 0; list < STAGELIST_MAX && !found; ++list) {
            for (int i = 0; i < stageListCount[list] && !found; ++i) {
                char name[0x40];
                std::snprintf(name, sizeof(name), "%s", stageList[list][i].name);
                for (char *end = name + std::strlen(name); end > name && end[-1] == ' '; --end)
                    end[-1] = 0;
                if (std::strcmp(name, wanted.name) == 0) {
                    g_screens.push_back({list, i, wanted.label});
                    found = true;
                }
            }
        }
    }
}

const char *const kSmoothing[] = {"Sharp pixels", "Smooth", nullptr};
const char *const kOnOff[] = {"Off", "On", nullptr};
#if RETRO_PLATFORM == RETRO_ANDROID
constexpr int kSettingCount = 3;
#else
constexpr int kSettingCount = 4;
#endif

int setting_get(int i, RsdkAirSetting *s)
{
    *s = RsdkAirSetting{};
    switch (i) {
    case 0: *s = {"Music volume", RSDKAIR_SETTING_RANGE, 0, MAX_VOLUME, 10, nullptr, bgmVolume}; return 1;
    case 1: *s = {"Sound volume", RSDKAIR_SETTING_RANGE, 0, MAX_VOLUME, 10, nullptr, sfxVolume}; return 1;
    case 2: *s = {"Smoothing", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kSmoothing, Engine.scalingMode ? 1 : 0}; return 1;
    case 3: *s = {"Fullscreen", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kOnOff, Engine.isFullScreen ? 1 : 0}; return 1;
    default: return 0;
    }
}

int clamp_volume(int v) { return v < 0 ? 0 : v > MAX_VOLUME ? MAX_VOLUME : v; }

void setting_set(int i, int v)
{
    switch (i) {
    case 0: bgmVolume = clamp_volume(v); break;
    case 1: sfxVolume = clamp_volume(v); break;
    case 2: Engine.scalingMode = v ? 1 : 0; break;
    case 3:
        Engine.startFullScreen = v != 0;
        SetFullScreen(v != 0);
        break;
    default: return;
    }
    WriteSettings();
}

void setup_menu(const RsdkAirHost *host)
{
    g_menu.host = host;
    RsdkAirEngineControl &c = g_menu.control;
    c.gfx = RSDKAIR_GFX_GL1;
    c.set_paused = [](int paused) {
        if (audioDevice > 0)
            SDL_PauseAudioDevice(audioDevice, paused);
    };
    c.request_exit = [] { Engine.running = false; };
    c.screen_count = [] {
        find_screens();
        return static_cast<int>(g_screens.size());
    };
    c.screen_name = [](int i) {
        return i >= 0 && i < static_cast<int>(g_screens.size()) ? g_screens[static_cast<std::size_t>(i)].label : "";
    };
    c.load_screen = [](int i) {
        if (i < 0 || i >= static_cast<int>(g_screens.size()))
            return;
        const Screen &s = g_screens[static_cast<std::size_t>(i)];
        StopMusic();
        StopAllSfx();
        ReleaseStageSfx();
        activeStageList = s.list;
        stageListPosition = s.stage;
        stageMode = STAGEMODE_LOAD;
        Engine.gameMode = ENGINE_MAINGAME;
    };
    c.setting_count = [] { return kSettingCount; };
    c.setting_get = setting_get;
    c.setting_set = setting_set;
}

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

// Called from the patched RetroEngine.cpp (engines/rsdkv3/CMakeLists.txt).
int rsdkair_rsdkv3_menu_event(const void *event) { return g_menu.event(event); }
void rsdkair_rsdkv3_menu_frame() { g_menu.frame(Engine.window); }

#if RETRO_PLATFORM == RETRO_OSX
// Upstream implements this in dependencies/mac/cocoaHelpers.mm (the app bundle's resources);
// the only caller left is the engine's log, which goes to the save folder like everything else.
void getResourcesPath(char *buffer, int bufferSize)
{
    std::snprintf(buffer, static_cast<std::size_t>(bufferSize), "%s", g_game_path.c_str());
}
#endif

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

    setup_menu(args->host);
    Engine.Init();
    if (!Engine.running)
        return RSDKAIR_EXIT_ENGINE_FAILED;
    g_menu.attach(Engine.window, true);  // the start screen: play, screens, settings
    if (Engine.running)                  // unless the player went back to the launcher from it
        Engine.Run();
    return RSDKAIR_EXIT_OK;
}

} // extern "C"
