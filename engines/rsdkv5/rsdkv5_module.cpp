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

#include "menu_glue.hpp"

#include <rsdkair/engine.h>
#include <rsdkair/ini.hpp>
#include <rsdkair/paths.hpp>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace {

const char *const kRequired[] = {"Data.rsdk", nullptr};
const RsdkAirGameInfo kMania = {"mania", "Sonic Mania", kRequired};
const RsdkAirGameInfo *const kGames[] = {&kMania, nullptr};

// RSDK::SKU::userFileDir is 0x100 bytes and the data pack path is built from it.
constexpr std::size_t kDataFileMax = 0x80;

std::string g_user_dir;

// ---- In-game menu (engines/common/menu_glue.hpp) ---------------------------------------
// No save slots yet (Mania's slot bridge is future work): Play, the game's screens,
// settings, back to the launcher. The start screen opens on the first frame after the
// engine loaded the game (its scene list), not over the mod loading screen.

rsdkair_menu::Glue g_menu;
unsigned g_pad = 0;  // on-screen controls held (RSDKAIR_PAD_*)
bool g_started = false;
std::filesystem::path g_rsdkair_ini;  // <save>/rsdk-air.ini: settings the engine has no key for
int g_smooth = 0;

// Mania's own screens, scenes of its "Presentation" category.
struct Screen {
    const char *scene;
    const char *label;
};
constexpr Screen kScreens[] = {{"Title Screen", "Title screen"}, {"Menu", "Main menu"}, {"Level Select", "Level select"}};
std::vector<const Screen *> g_screens;
bool g_screens_found = false;

void find_screens()
{
    if (g_screens_found || RSDK::sceneInfo.categoryCount == 0)
        return;
    g_screens_found = true;
    for (int c = 0; c < RSDK::sceneInfo.categoryCount; ++c) {
        const RSDK::SceneListInfo &cat = RSDK::sceneInfo.listCategory[c];
        if (std::strcmp(cat.name, "Presentation") != 0)
            continue;
        for (const Screen &screen : kScreens) {
            for (int s = 0; s < cat.sceneCount; ++s) {
                if (std::strcmp(RSDK::sceneInfo.listData[cat.sceneOffsetStart + s].name, screen.scene) == 0) {
                    g_screens.push_back(&screen);
                    break;
                }
            }
        }
    }
}

void apply_smoothing()
{
    for (int i = 0; i < SCREEN_COUNT; ++i) {
        if (RSDK::RenderDevice::screenTexture[i])
            SDL_SetTextureScaleMode(RSDK::RenderDevice::screenTexture[i], g_smooth ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
    }
}

const char *const kSmoothing[] = {"Sharp pixels", "Smooth", nullptr};
const char *const kOnOff[] = {"Off", "On", nullptr};
#if defined(__ANDROID__)
constexpr int kSettingCount = 3;
#else
constexpr int kSettingCount = 4;
#endif

int setting_get(int i, RsdkAirSetting *s)
{
    *s = RsdkAirSetting{};
    const int music = static_cast<int>(RSDK::engine.streamVolume * 100.0f + 0.5f);
    const int sound = static_cast<int>(RSDK::engine.soundFXVolume * 100.0f + 0.5f);
    switch (i) {
    case 0: *s = {"Music volume", RSDKAIR_SETTING_RANGE, 0, 100, 10, nullptr, music}; return 1;
    case 1: *s = {"Sound volume", RSDKAIR_SETTING_RANGE, 0, 100, 10, nullptr, sound}; return 1;
    case 2: *s = {"Smoothing", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kSmoothing, g_smooth}; return 1;
    case 3: *s = {"Fullscreen", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kOnOff, RSDK::videoSettings.windowed ? 0 : 1}; return 1;
    default: return 0;
    }
}

void setting_set(int i, int v)
{
    switch (i) {
    case 0: RSDK::engine.streamVolume = static_cast<float>(v) / 100.0f; break;
    case 1: RSDK::engine.soundFXVolume = static_cast<float>(v) / 100.0f; break;
    case 2:
        g_smooth = v ? 1 : 0;
        apply_smoothing();
        rsdkair::ini_set_file(g_rsdkair_ini, "Video", "Smooth", g_smooth ? "true" : "false");
        return;
    case 3:
        // Not the engine's UpdateGameWindow(): it re-creates the renderer the menu is
        // drawing with. The window changes in place; the logical size rescales.
        RSDK::videoSettings.windowed = v ? 0 : 1;
        SDL_SetWindowFullscreen(RSDK::RenderDevice::window, v ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        break;
    default: return;
    }
    RSDK::SaveSettingsINI(true);
}

void setup_menu(const RsdkAirHost *host)
{
    g_menu.host = host;
    RsdkAirEngineControl &c = g_menu.control;
    c.gfx = RSDKAIR_GFX_SDL_RENDERER;
    c.set_paused = [](int paused) {
        if (RSDK::AudioDevice::device > 0)
            SDL_PauseAudioDevice(RSDK::AudioDevice::device, paused);
    };
    c.request_exit = [] { RSDK::RenderDevice::isRunning = false; };
    c.screen_count = [] {
        find_screens();
        return static_cast<int>(g_screens.size());
    };
    c.screen_name = [](int i) {
        return i >= 0 && i < static_cast<int>(g_screens.size()) ? g_screens[static_cast<std::size_t>(i)]->label : "";
    };
    c.load_screen = [](int i) {
        if (i < 0 || i >= static_cast<int>(g_screens.size()))
            return;
        RSDK::SetScene("Presentation", g_screens[static_cast<std::size_t>(i)]->scene);
        RSDK::LoadScene();
    };
    c.setting_count = [] { return kSettingCount; };
    c.setting_get = setting_get;
    c.setting_set = setting_set;
    c.set_pad = [](unsigned buttons) { g_pad = buttons; };
    c.hide_touch_controls = [](int) {};  // Mania has no touch controls of its own
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

// Called from the patched Input.cpp: the on-screen controls' buttons, as raw "held"
// states for player 1 and "any" (the engine then sets down/press as for a pad).
void rsdkair_rsdkv5_apply_pad()
{
    if (!g_pad)
        return;
    for (int c : {static_cast<int>(RSDK::CONT_ANY), static_cast<int>(RSDK::CONT_P1)}) {
        RSDK::ControllerState &s = RSDK::controller[c];
        if (g_pad & RSDKAIR_PAD_UP)
            s.keyUp.press = true;
        if (g_pad & RSDKAIR_PAD_DOWN)
            s.keyDown.press = true;
        if (g_pad & RSDKAIR_PAD_LEFT)
            s.keyLeft.press = true;
        if (g_pad & RSDKAIR_PAD_RIGHT)
            s.keyRight.press = true;
        if (g_pad & RSDKAIR_PAD_A)
            s.keyA.press = true;
    }
}

// Called from the patched SDL2RenderDevice.cpp (engines/rsdkv5/CMakeLists.txt).
int rsdkair_rsdkv5_menu_event(const void *event) { return g_menu.event(event); }

void rsdkair_rsdkv5_menu_frame()
{
    g_menu.control.renderer = RSDK::RenderDevice::renderer;
    if (!g_started && RSDK::sceneInfo.categoryCount > 0) {
        // The game is loaded: its start screen, once, over the first real frame.
        g_started = true;
        apply_smoothing();
        g_menu.attach(RSDK::RenderDevice::window, true);
        return;
    }
    if (g_started)
        g_menu.frame(RSDK::RenderDevice::window);
}

// Where InitUserDirectory points the engine on macOS and Android (patched UserStorage.cpp).
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

#if defined(__ANDROID__)
    // Fullscreen on Android, as upstream's RETRO_ANDROID branch of the SDL2 render device
    // does (skipped here: the module is built as the Linux flavour). In a window the engine
    // draws into its 424x240 window size in a corner of the screen.
    if (!rsdkair::ini_set_file(settings, "Video", "windowed", "n") ||
        !rsdkair::ini_set_file(settings, "Video", "exclusiveFS", "n")) {
        std::fprintf(stderr, "rsdkv5: cannot write %s\n", rsdkair::path_to_utf8(settings).c_str());
        return RSDKAIR_EXIT_ENGINE_FAILED;
    }
#endif

    // Linux ("./") and Windows ("") resolve the user file directory against the working
    // directory; macOS and Android get the folder itself (patched UserStorage.cpp).
    g_user_dir = rsdkair::path_to_utf8(save_dir) + "/";
    fs::current_path(save_dir, ec);
    if (ec) {
        std::fprintf(stderr, "rsdkv5: cannot enter %s\n", rsdkair::path_to_utf8(save_dir).c_str());
        return RSDKAIR_EXIT_ENGINE_FAILED;
    }

    // rsdk-air's own settings for this game (none of the engine's keys fit them).
    g_rsdkair_ini = save_dir / "rsdk-air.ini";
    {
        std::ifstream in(g_rsdkair_ini);
        const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        g_smooth = rsdkair::ini_get(text, "Video", "Smooth") == "true" ? 1 : 0;
    }
    setup_menu(args->host);

    // What upstream's RSDK_main (main.cpp) does, with the game logic linked statically.
    RSDK::linkGameLogic = reinterpret_cast<RSDK::LogicLinkHandle>(&LinkGameLogicDLL);
    RSDK::InitCoreAPI();
    const int code = RSDK::RunRetroEngine(0, nullptr);
    RSDK::ReleaseCoreAPI();
    return code == 0 ? RSDKAIR_EXIT_OK : RSDKAIR_EXIT_ENGINE_FAILED;
}

} // extern "C"
