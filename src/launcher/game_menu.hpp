// The in-game menu: the same screen when a game opens and when the player pauses it.
//
// Runs in the game process, inside the engine's own hooks (engine ABI 2, RsdkAirHost in
// include/rsdkair/engine.h). It draws over the engine's window with Dear ImGui, through
// the engine's own graphics: fixed-function OpenGL 1.x / ES 1.x (RSDKv3, RSDKv4) or an
// SDL_Renderer (RSDKv5). While it is open the engine is blocked in the hook, so the
// game is frozen; the last frame stays underneath, dimmed.
//
// What it holds (ROADMAP/2026-11-20/game-menus.md, rsdkv4-wasm's app):
//   - the save slots, where the engine has a slot bridge (Sonic 1 and 2): NO SAVE and
//     four slots, as craft's cards; continue, new game with a character, delete;
//   - Continue (when paused), or Play / Title screen (when the game opens);
//   - the game's own special screens (level select, stage menu…), found in its data;
//   - Settings (volumes, smoothing, fullscreen), applied and saved by the engine;
//   - Back to the launcher (asks first when a game is running).
// Input: arrows / d-pad / left stick, A / Enter, B / Escape, Y / Delete; touch and mouse.
// Opened by Escape, the Android back button, or Select + Start (or Guide) on a pad.
#pragma once

#include <rsdkair/engine.h>

#include <memory>
#include <string>

namespace rsdkair {

class GameMenu {
public:
    // game_id and title as in the catalog (art/covers/<game_id>.jpg is the backdrop
    // before the first frame).
    GameMenu(std::string game_id, std::string title);
    ~GameMenu();
    GameMenu(const GameMenu &) = delete;
    GameMenu &operator=(const GameMenu &) = delete;

    // The hooks to hand to the engine module (EngineRun::host).
    const RsdkAirHost *host() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rsdkair
