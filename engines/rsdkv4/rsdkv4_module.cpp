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
#include "menu_glue.hpp"
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
#include <vector>

namespace fs = std::filesystem;

extern SDL_AudioDeviceID audioDevice;  // RSDKv4's Audio.cpp (not in its headers)

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

// ---- In-game menu (engines/common/menu_glue.hpp) ---------------------------------------

rsdkair_menu::Glue g_menu;
unsigned g_pad = 0;            // on-screen controls held (RSDKAIR_PAD_*)
bool g_touch_hidden = false;   // our controls replace the game's VirtualDPad

// The packs' own menu screens, by their name in the stage lists (rsdkv4-wasm's
// PACK_SCREENS): {list, position, label}.
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
    } kWanted[] = {{"TITLE SCREEN", "Title screen"}, {"LEVEL SELECT", "Level select"}, {"STAGE MENU", "Stage menu"}};
    for (const auto &wanted : kWanted) {
        for (int list = 0; list < STAGELIST_MAX; ++list) {
            for (int i = 0; i < stageListCount[list]; ++i) {
                char name[0x40];
                std::snprintf(name, sizeof(name), "%s", stageList[list][i].name);
                for (char *end = name + std::strlen(name); end > name && end[-1] == ' '; --end)
                    end[-1] = 0;
                if (std::strcmp(name, wanted.name) == 0) {
                    g_screens.push_back({list, i, wanted.label});
                    list = STAGELIST_MAX;  // the first match only
                    break;
                }
            }
        }
    }
}

// Settings, saved in the engine's settings.ini by its own WriteSettings(). Language and
// screen width are read only when the engine starts: they are written to settings.ini
// (again after each WriteSettings(), which would write the running values back) and the
// engine picks them up the next time the game starts.
std::filesystem::path g_settings_ini;
int g_language = -1;      // chosen for the next start, -1 none
int g_screen_width = -1;  // chosen for the next start, -1 none

const char *const kSmoothing[] = {"Sharp pixels", "Smooth", nullptr};
const char *const kOnOff[] = {"Off", "On", nullptr};
// The engine knows 11 (RETRO_EN..RETRO_ZS), but a language missing from the game's
// StringList.txt leaves its menus empty: only the six every release has.
const char *const kLanguages[] = {"English", "French", "Italian", "German", "Spanish", "Japanese", nullptr};
constexpr int kLastLanguage = RETRO_JP;
const char *const kWidths[] = {"4:3 (320)", "Widescreen (424)", nullptr};
const int kWidthValues[] = {320, 424};
const char *const kScales[] = {"1x", "2x", "3x", "4x", nullptr};

enum Setting { Music, Sound, Language, Smoothing, Fullscreen, VSync, WindowSize, ScreenWidth, FocusPause };
#if RETRO_PLATFORM == RETRO_ANDROID
const Setting kSettings[] = {Music, Sound, Language, Smoothing};
#else
const Setting kSettings[] = {Music, Sound, Language, Smoothing, Fullscreen, VSync, WindowSize, ScreenWidth, FocusPause};
#endif
constexpr int kSettingCount = static_cast<int>(sizeof(kSettings) / sizeof(kSettings[0]));

// DisableFocusPause bits: 1 the game's pause, 2 the engine's (RSDKv4's RetroEngine.cpp).
constexpr int kNoFocusPause = 3;

int width_choice(int width) { return width <= 360 ? 0 : 1; }

int setting_get(int i, RsdkAirSetting *s)
{
    *s = RsdkAirSetting{};
    if (i < 0 || i >= kSettingCount)
        return 0;
    switch (kSettings[i]) {
    case Music: *s = {"Music volume", RSDKAIR_SETTING_RANGE, 0, MAX_VOLUME, 10, nullptr, bgmVolume, 0}; return 1;
    case Sound: *s = {"Sound volume", RSDKAIR_SETTING_RANGE, 0, MAX_VOLUME, 10, nullptr, sfxVolume, 0}; return 1;
    case Language: {
        const int lang = g_language >= 0 ? g_language : Engine.language;
        *s = {"Language", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kLanguages, lang >= 0 && lang <= kLastLanguage ? lang : 0, 1};
        return 1;
    }
    case Smoothing: *s = {"Smoothing", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kSmoothing, Engine.scalingMode ? 1 : 0, 0}; return 1;
    case Fullscreen: *s = {"Fullscreen", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kOnOff, Engine.isFullScreen ? 1 : 0, 0}; return 1;
    case VSync: *s = {"VSync", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kOnOff, Engine.vsync ? 1 : 0, 0}; return 1;
    case WindowSize: {
        const int scale = Engine.windowScale < 1 ? 1 : Engine.windowScale > 4 ? 4 : Engine.windowScale;
        *s = {"Window size", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kScales, scale - 1, 0};
        return 1;
    }
    case ScreenWidth: {
        const int width = g_screen_width >= 0 ? g_screen_width : SCREEN_XSIZE_CONFIG;
        *s = {"Screen width", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kWidths, width_choice(width), 1};
        return 1;
    }
    case FocusPause:
        *s = {"Pause when in background", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kOnOff, disableFocusPause_Config == 0 ? 1 : 0, 0};
        return 1;
    }
    return 0;
}

// The choices for the next start, over what the engine wrote.
void apply_pending()
{
    if (g_settings_ini.empty())
        return;
    if (g_language >= 0)
        rsdkair::ini_set_file(g_settings_ini, "Game", "Language", std::to_string(g_language));
    if (g_screen_width >= 0)
        rsdkair::ini_set_file(g_settings_ini, "Window", "ScreenWidth", std::to_string(g_screen_width));
}

void save_settings()
{
    WriteSettings();
    apply_pending();
}

void setting_set(int i, int v)
{
    if (i < 0 || i >= kSettingCount)
        return;
    switch (kSettings[i]) {
    case Music: SetGameVolumes(v, sfxVolume); break;
    case Sound: SetGameVolumes(bgmVolume, v); break;
    case Language: g_language = v < 0 ? 0 : v > kLastLanguage ? kLastLanguage : v; break;
    case Smoothing: Engine.scalingMode = v ? 1 : 0; break;  // the renderer picks the filter every frame
    case Fullscreen:
        Engine.startFullScreen = v != 0;
        SetFullScreen(v != 0);
        break;
    case VSync:
        // The engine's GL context is current here (the menu runs inside its frame).
        Engine.vsync = v != 0;
        SDL_GL_SetSwapInterval(Engine.vsync ? 1 : 0);
        break;
    case WindowSize:
        Engine.windowScale = (v < 0 ? 0 : v > 3 ? 3 : v) + 1;
        if (!Engine.isFullScreen)
            SetFullScreen(false);  // resizes the window and the viewport
        break;
    case ScreenWidth: g_screen_width = kWidthValues[v ? 1 : 0]; break;
    case FocusPause:
        disableFocusPause_Config = v ? 0 : kNoFocusPause;
        disableFocusPause = disableFocusPause_Config;
        break;
    }
    save_settings();
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
    c.screen_name = [](int i) { return i >= 0 && i < static_cast<int>(g_screens.size()) ? g_screens[static_cast<std::size_t>(i)].label : ""; };
    c.load_screen = [](int i) {
        if (i < 0 || i >= static_cast<int>(g_screens.size()))
            return;
        const Screen &s = g_screens[static_cast<std::size_t>(i)];
        // As rsdkv4-wasm's web_devmenu_load_stage: silence the stage being left, then
        // load the screen through the engine's own stage loader.
        StopMusic(true);
        StopAllSfx();
        ReleaseStageSfx();
        activeStageList = s.list;
        stageListPosition = s.stage;
        stageMode = STAGEMODE_LOAD;
        rsdkv4_bridge::enter_game_loop();  // also when the native title or menus are running
        SetGlobalVariableByName("options.gameMode", 0);
        SetGlobalVariableByName("lampPostID", 0); // Sonic 1
        SetGlobalVariableByName("starPostID", 0); // Sonic 2
    };
    c.setting_count = [] { return kSettingCount; };
    c.setting_get = setting_get;
    c.setting_set = setting_set;
    c.slots_kind = RSDKAIR_SLOTS_RSDKV4;
    c.read_save_ram = [](int *out, int count) {
        const int n = count < 0x2000 ? count : 0x2000;
        for (int i = 0; i < n; ++i)
            out[i] = saveRAM[i];
        return n;
    };
    c.start_slot = [](int slot, int player) { rsdkv4_bridge::start_game(slot, player); };
    c.delete_slot = [](int slot) { rsdkv4_bridge::delete_save(slot); };
    c.set_pad = [](unsigned buttons) { g_pad = buttons; };
    c.hide_touch_controls = [](int hide) { g_touch_hidden = hide != 0; };
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

// Called from the patched RetroEngine.cpp (engines/rsdkv4/CMakeLists.txt).
int rsdkair_rsdkv4_menu_event(const void *event) { return g_menu.event(event); }
void rsdkair_rsdkv4_menu_frame() { g_menu.frame(Engine.window); }
void rsdkair_rsdkv4_menu_open() { g_menu.open(); }

// On-screen controls (patched Input.cpp, VirtualDPad.cpp, VirtualDPadM.cpp). Input
// buttons 0-4 are up, down, left, right, A: RSDKAIR_PAD_* bit by bit.
bool rsdkair_rsdkv4_pad_held(int button) { return button >= 0 && button <= 4 && (g_pad & (1u << button)) != 0; }
bool rsdkair_rsdkv4_touch_hidden() { return g_touch_hidden; }

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
    const fs::path settings = fs::absolute(save_dir / "settings.ini", ec);
    g_settings_ini = settings;
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

    setup_menu(args->host);
    Engine.Init();
    if (!Engine.running)
        return RSDKAIR_EXIT_ENGINE_FAILED;

    if (start.has_slot)
        rsdkv4_bridge::start_game(start.slot, start.player);
    else
        g_menu.attach(Engine.window, true);  // the start screen: slots, screens, settings

    if (Engine.running)  // unless the player went back to the launcher from it
        Engine.Run();
    apply_pending();  // the engine writes its settings when it quits
    return RSDKAIR_EXIT_OK;
}

} // extern "C"
