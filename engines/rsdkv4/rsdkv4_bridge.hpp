// The save-slot bridge: starts a game in a slot the way RSDKv4's own Start Menu does
// (Debug.cpp, STARTMENU_SAVESEL). Ported from rsdkv4-wasm's WebGame.cpp, minus the
// JavaScript bindings.
//
// SPDX-License-Identifier: MIT (this file); it links against the RSDKv4 decompilation.
#pragma once

namespace rsdkv4_bridge {

// slot 0-3 with data: continue it. slot 0-3 empty: new game there as `player`.
// slot < 0: no-save game as `player` (does not touch the save file).
// player: 0 Sonic, 1 Tails, 2 Knuckles, 3 Sonic & Tails.
void start_game(int slot, int player);

// Clears a slot and writes the save file, as the engine's own delete does.
void delete_save(int slot);

// Hands the screen to the game loop, as the game's own menus do when they start a game
// (FadeScreen, PauseMenu): the native objects (splash, title, menus) are replaced by
// RetroGameLoop, plus VirtualDPad on mobile. Without it a stage set up while the native
// start menu runs never shows: the engine keeps running its splash and menus.
void enter_game_loop();

} // namespace rsdkv4_bridge
