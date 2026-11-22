// rsdkair_menu_demo: a stand-in engine for trying the in-game menu (game_menu.cpp)
// without game data. Built with the tests, never installed.
//
//   rsdkair_menu_demo gl1 [--no-slots]   OpenGL 1.x fixed function, as RSDKv3/v4
//   rsdkair_menu_demo sdl [--no-slots]   SDL_Renderer with a logical size, as RSDKv5
//
// It draws a moving test pattern with the same kind of state the engines leave behind,
// calls the menu's hooks as a patched engine does, and offers fake settings, screens and
// Sonic 2 save slots. Escape opens the menu; leaving it ends the program.
#include "game_menu.hpp"

#include <rsdkair/saves_rsdkv4.hpp>

#include <SDL.h>
#include <SDL_opengl.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

bool g_running = true;
int g_music = 80, g_sound = 100, g_smooth = 0, g_fullscreen = 0;
std::vector<int> g_ram(rsdkair::rsdkv4::kSaveRamInts, 0);

const char *const kSmooth[] = {"Sharp pixels", "Smooth", nullptr};
const char *const kOnOff[] = {"Off", "On", nullptr};
const char *const kScreens[] = {"Level select", "Stage menu"};

int setting_count() { return 4; }

int setting_get(int i, RsdkAirSetting *s)
{
    *s = RsdkAirSetting{};
    switch (i) {
    case 0: *s = {"Music volume", RSDKAIR_SETTING_RANGE, 0, 100, 10, nullptr, g_music}; return 1;
    case 1: *s = {"Sound volume", RSDKAIR_SETTING_RANGE, 0, 100, 10, nullptr, g_sound}; return 1;
    case 2: *s = {"Smoothing", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kSmooth, g_smooth}; return 1;
    case 3: *s = {"Fullscreen", RSDKAIR_SETTING_CHOICE, 0, 0, 0, kOnOff, g_fullscreen}; return 1;
    default: return 0;
    }
}

void setting_set(int i, int v)
{
    int *targets[] = {&g_music, &g_sound, &g_smooth, &g_fullscreen};
    if (i >= 0 && i < 4)
        *targets[i] = v;
    std::printf("setting %d = %d\n", i, v);
}

int read_save_ram(int *out, int count)
{
    for (int i = 0; i < count && i < static_cast<int>(g_ram.size()); ++i)
        out[i] = g_ram[static_cast<std::size_t>(i)];
    return count;
}

void start_slot(int slot, int player) { std::printf("start slot %d as %d\n", slot, player); }
void delete_slot(int slot)
{
    for (int i = 0; i < 8; ++i)
        g_ram[static_cast<std::size_t>(slot * 8 + i)] = 0;
    std::printf("delete slot %d\n", slot);
}

} // namespace

int main(int argc, char **argv)
{
    const bool gl = argc > 1 && std::strcmp(argv[1], "gl1") == 0;
    const bool slots = !(argc > 2 && std::strcmp(argv[2], "--no-slots") == 0);
    // A Sonic 2 save: slot 1 Tails in Aquatic Ruin 2, slot 3 Sonic & Tails in a special stage.
    const int s1[8] = {1, 4, 23450, 50000, 6, 2, 2, 0}, s3[8] = {3, 7, 98760, 50000, 131, 7, 3, 0};
    std::memcpy(&g_ram[0], s1, sizeof(s1));
    std::memcpy(&g_ram[16], s3, sizeof(s3));

    // Mouse clicks also arrive as touches, to try the on-screen controls on a desktop
    // (enable them in menu_demo.ini: [Touch] Enabled=true).
    SDL_SetHint(SDL_HINT_MOUSE_TOUCH_EVENTS, "1");
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER);
    if (gl) {
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
        SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
        SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    }
    SDL_Window *window = SDL_CreateWindow("rsdk-air", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720,
                                          SDL_WINDOW_RESIZABLE | (gl ? SDL_WINDOW_OPENGL : 0));
    if (!window) {
        std::fprintf(stderr, "cannot create the window: %s\n", SDL_GetError());
        return 1;
    }
    SDL_GLContext context = gl ? SDL_GL_CreateContext(window) : nullptr;
    if (gl && !context) {
        std::fprintf(stderr, "cannot create the OpenGL context: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer *renderer = gl ? nullptr : SDL_CreateRenderer(window, -1, 0);
    SDL_Texture *screen = nullptr;
    if (renderer) {
        SDL_RenderSetLogicalSize(renderer, 424, 240);
        screen = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, 424, 240);
    }

    rsdkair::GameMenu menu("sonic2", "Sonic the Hedgehog 2", "menu_demo.ini");
    const RsdkAirHost *host = menu.host();
    RsdkAirEngineControl control{};
    control.window = window;
    control.gfx = gl ? RSDKAIR_GFX_GL1 : RSDKAIR_GFX_SDL_RENDERER;
    control.renderer = renderer;
    control.set_paused = [](int p) { std::printf("paused %d\n", p); };
    control.request_exit = [] { g_running = false; };
    control.screen_count = [] { return 2; };
    control.screen_name = [](int i) { return kScreens[i]; };
    control.load_screen = [](int i) { std::printf("load screen %d\n", i); };
    control.setting_count = setting_count;
    control.setting_get = setting_get;
    control.setting_set = setting_set;
    control.set_pad = [](unsigned b) {
        static unsigned last = 0;
        if (b != last)
            std::printf("pad %02x\n", b);
        last = b;
        std::fflush(stdout);
    };
    control.hide_touch_controls = [](int hide) { std::printf("hide game touch controls %d\n", hide); };
    if (slots) {
        control.slots_kind = RSDKAIR_SLOTS_RSDKV4;
        control.read_save_ram = read_save_ram;
        control.start_slot = start_slot;
        control.delete_slot = delete_slot;
    }

    host->attach(host->ctx, &control, 1);  // the start screen, before the first frame

    std::vector<Uint16> pixels(424 * 240);
    int frame = 0;
    while (g_running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (host->event(host->ctx, &e))
                continue;
            if (e.type == SDL_QUIT)
                g_running = false;
        }
        ++frame;
        const float t = static_cast<float>(frame) / 60.0f;
        if (gl) {
            int w = 0, h = 0;
            SDL_GL_GetDrawableSize(window, &w, &h);
            glViewport(0, 0, w, h);
            glClearColor(0.1f, 0.3f + 0.1f * std::sin(t), 0.6f, 1.0f);
            glClear(GL_COLOR_BUFFER_BIT);
            // Engine-like state: its own projection, left in place between frames.
            glMatrixMode(GL_PROJECTION);
            glLoadIdentity();
            glOrtho(0, 424, 240, 0, -1, 1);
            glMatrixMode(GL_MODELVIEW);
            glLoadIdentity();
            glDisable(GL_TEXTURE_2D);
            for (int i = 0; i < 12; ++i) {
                const float x = std::fmod(static_cast<float>(i) * 40.0f + t * 60.0f, 464.0f) - 40.0f;
                glColor4f(i % 2 ? 1.0f : 0.2f, 0.8f, i % 3 ? 0.2f : 1.0f, 1.0f);
                const GLfloat quad[] = {x, 150, x + 30, 150, x + 30, 210, x, 210};
                glEnableClientState(GL_VERTEX_ARRAY);
                glVertexPointer(2, GL_FLOAT, 0, quad);
                glDrawArrays(GL_TRIANGLE_FAN, 0, 4);
            }
            host->frame(host->ctx);
            SDL_GL_SwapWindow(window);
        } else {
            for (int y = 0; y < 240; ++y) {
                for (int x = 0; x < 424; ++x) {
                    const int v = ((x + frame) / 16 + y / 16) & 1;
                    pixels[static_cast<std::size_t>(y * 424 + x)] = v ? 0x2A7F : static_cast<Uint16>(0xFD20 + (y & 0x1F));
                }
            }
            SDL_UpdateTexture(screen, nullptr, pixels.data(), 424 * 2);
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, screen, nullptr, nullptr);
            host->frame(host->ctx);
            SDL_RenderPresent(renderer);
        }
        SDL_Delay(16);
    }
    if (context)
        SDL_GL_DeleteContext(context);
    if (renderer)
        SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
