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
std::filesystem::path g_settings_ini;  // <save>/Settings.ini, the engine's own
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

// Settings, saved in the engine's Settings.ini by its own SaveSettingsINI() (smoothing in
// rsdk-air.ini). Language and screen width are read only when the engine starts: they are
// written to Settings.ini (again after each save and when the engine quits, as it writes
// the running values back) and the engine picks them up the next time the game starts.
// The screen width also stays in rsdk-air.ini: SaveSettingsINI() drops pixWidth when the
// file already has it, so it is put back before every start.
int g_language = -1;   // chosen for the next start, -1 none
int g_pix_width = -1;  // chosen (now or in an earlier session), -1 none

const char *const kSmoothing[] = {"Sharp pixels", "Smooth", nullptr};
const char *const kOnOff[] = {"Off", "On", nullptr};
const char *const kLanguages[] = {"English",  "French", "Italian", "German", "Spanish", "Japanese", "Korean",
                                  "Chinese (Simplified)", "Chinese (Traditional)", nullptr};
constexpr int kLastLanguage = RSDK::LANGUAGE_TC;
const char *const kWidths[] = {"4:3 (320)", "Widescreen (424)", nullptr};
const int kWidthValues[] = {320, DEFAULT_PIXWIDTH};
const char *const kScales[] = {"1x", "2x", "3x", "4x", nullptr};

enum Setting { Music, Sound, Language, Smoothing, Fullscreen, VSync, WindowSize, ScreenWidth, FocusPause };
#if defined(__ANDROID__)
const Setting kSettings[] = {Music, Sound, Language, Smoothing};
#else
const Setting kSettings[] = {Music, Sound, Language, Smoothing, Fullscreen, VSync, WindowSize, ScreenWidth, FocusPause};
#endif
constexpr int kSettingCount = static_cast<int>(sizeof(kSettings) / sizeof(kSettings[0]));

int width_choice(int width) { return width <= 360 ? 0 : 1; }

int current_language()
{
#if RETRO_REV02
    return RSDK::SKU::curSKU.language;
#else
    return RSDK::gameVerInfo.language;
#endif
}

int window_scale()
{
    const int scale = (RSDK::videoSettings.windowHeight + SCREEN_YSIZE / 2) / SCREEN_YSIZE;
    return scale < 1 ? 1 : scale > 4 ? 4 : scale;
}

int setting_get(int i, RsdkAirSetting *s)
{
    *s = RsdkAirSetting{};
    if (i < 0 || i >= kSettingCount)
        return 0;
    const int music = static_cast<int>(RSDK::engine.streamVolume * 100.0f + 0.5f);
    const int sound = static_cast<int>(RSDK::engine.soundFXVolume * 100.0f + 0.5f);
    switch (kSettings[i]) {
    case Music: *s = {"Music volume", RSDKAIR_SETTING_RANGE, 0, 100, 10, nullptr, music, 0}; return 1;
    case Sound: *s = {"Sound volume", RSDKAIR_SETTING_RANGE, 0, 100, 10, nullptr, sound, 0}; return 1;
    case Language: {
        const int lang = g_language >= 0 ? g_language : current_language();
        *s = {"Language", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kLanguages, lang >= 0 && lang <= kLastLanguage ? lang : 0, 1};
        return 1;
    }
    case Smoothing: *s = {"Smoothing", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kSmoothing, g_smooth, 0}; return 1;
    case Fullscreen:
        *s = {"Fullscreen", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kOnOff, RSDK::videoSettings.windowed ? 0 : 1, 0};
        return 1;
    case VSync: *s = {"VSync", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kOnOff, RSDK::videoSettings.vsync ? 1 : 0, 0}; return 1;
    case WindowSize: *s = {"Window size", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kScales, window_scale() - 1, 0}; return 1;
    case ScreenWidth: {
        const int width = g_pix_width >= 0 ? g_pix_width : RSDK::videoSettings.pixWidth;
        *s = {"Screen width", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kWidths, width_choice(width), 1};
        return 1;
    }
    case FocusPause:
        *s = {"Pause when in background", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kOnOff,
              RSDK::customSettings.disableFocusPause ? 0 : 1, 0};
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
        rsdkair::ini_set_file(g_settings_ini, "Game", "language", std::to_string(g_language));
    if (g_pix_width >= 0)
        rsdkair::ini_set_file(g_settings_ini, "Video", "pixWidth", std::to_string(g_pix_width));
}

void setting_set(int i, int v)
{
    if (i < 0 || i >= kSettingCount)
        return;
    switch (kSettings[i]) {
    case Music: RSDK::engine.streamVolume = static_cast<float>(v) / 100.0f; break;
    case Sound: RSDK::engine.soundFXVolume = static_cast<float>(v) / 100.0f; break;
    case Language: g_language = v < 0 ? 0 : v > kLastLanguage ? kLastLanguage : v; break;
    case Smoothing:
        g_smooth = v ? 1 : 0;
        apply_smoothing();
        rsdkair::ini_set_file(g_rsdkair_ini, "Video", "Smooth", g_smooth ? "true" : "false");
        return;
    case Fullscreen:
        // Not the engine's UpdateGameWindow(): it re-creates the renderer the menu is
        // drawing with. The window changes in place; the logical size rescales.
        RSDK::videoSettings.windowed = v ? 0 : 1;
        SDL_SetWindowFullscreen(RSDK::RenderDevice::window, v ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        break;
    case VSync:
        // In place, as fullscreen; the engine's own 60 FPS cap stays on either way.
        RSDK::videoSettings.vsync = v ? 1 : 0;
        SDL_RenderSetVSync(RSDK::RenderDevice::renderer, v ? 1 : 0);
        break;
    case WindowSize: {
        // In place too: the window is resized and the logical size rescales.
        const int scale = (v < 0 ? 0 : v > 3 ? 3 : v) + 1;
        RSDK::videoSettings.windowWidth = RSDK::videoSettings.pixWidth * scale;
        RSDK::videoSettings.windowHeight = SCREEN_YSIZE * scale;
        if (RSDK::videoSettings.windowed) {
            SDL_SetWindowSize(RSDK::RenderDevice::window, RSDK::videoSettings.windowWidth, RSDK::videoSettings.windowHeight);
            SDL_SetWindowPosition(RSDK::RenderDevice::window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        }
        break;
    }
    case ScreenWidth:
        g_pix_width = kWidthValues[v ? 1 : 0];
        rsdkair::ini_set_file(g_rsdkair_ini, "Video", "PixWidth", std::to_string(g_pix_width));
        break;
    case FocusPause: RSDK::customSettings.disableFocusPause = v ? false : true; break;
    }
    RSDK::SaveSettingsINI(true);
    apply_pending();
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
    const fs::path settings = fs::absolute(save_dir / "Settings.ini", ec);
    g_settings_ini = settings;
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
        const std::string pix_width = rsdkair::ini_get(text, "Video", "PixWidth");
        if (pix_width == "320" || pix_width == "424")
            g_pix_width = std::stoi(pix_width);
    }
    apply_pending();  // the screen width chosen earlier, before the engine reads it
    setup_menu(args->host);

    // What upstream's RSDK_main (main.cpp) does, with the game logic linked statically.
    RSDK::linkGameLogic = reinterpret_cast<RSDK::LogicLinkHandle>(&LinkGameLogicDLL);
    RSDK::InitCoreAPI();
    const int code = RSDK::RunRetroEngine(0, nullptr);
    RSDK::ReleaseCoreAPI();
    apply_pending();  // the engine may write its settings when it quits
    return code == 0 ? RSDKAIR_EXIT_OK : RSDKAIR_EXIT_ENGINE_FAILED;
}

} // extern "C"
