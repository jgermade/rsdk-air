// RSDKv4 player-list names, for the in-game menu and the rsdkv4 module (which builds with
// C++17, like its engine, so this header stays C++17).
//
// SPDX-License-Identifier: MIT
#pragma once

#include <string>
#include <string_view>

namespace rsdkair::rsdkv4 {

// A character as the game's player list names it (GameConfig, e.g. "SONIC AND TAILS",
// "AMY"), for the menu: a label ("Sonic & Tails"), the picture under art/
// ("players/sonictails.png"; empty for a character with none), and whether Tails comes
// along as player 2 (stage.player2Enabled). Known names: Sonic, Tails (also Miles),
// Knuckles, Amy, Mighty, Ray; a pair is "<name> AND/&/+/WITH TAILS". Anything else keeps
// its own name, title-cased, with no picture.
struct CharacterLook {
    std::string label;
    std::string art;
    bool with_tails = false;
};
CharacterLook character_look(std::string_view engine_name);

} // namespace rsdkair::rsdkv4
