// The game picker: a window with a mosaic of game covers, in the style of play.germade.
//
// Runs in the launcher process only. It never loads an engine module: it shows the
// catalog with what detect_games() found and returns the player's choice.
//
// Input (engine-specs UX.md): arrows / d-pad / left stick move the focus, A / Enter / Space
// plays, B / Escape asks whether to quit (No focused). Mouse and touch: a tap focuses a
// cover, a tap on the focused cover (or on Play) plays it.
//
// Art comes from <resources>/art/covers/<id>.jpg and fonts from <resources>/fonts/; any
// missing file falls back to neutral art and Dear ImGui's built-in font.
#pragma once

#include <rsdkair/detect.hpp>

#include <memory>
#include <string>
#include <vector>

namespace rsdkair {

class Picker {
public:
    Picker();
    ~Picker();
    Picker(const Picker &) = delete;
    Picker &operator=(const Picker &) = delete;

    // Opens the window. False (with error set) if no window or renderer can be created.
    bool open(std::string &error);

    // Shows `games` until the player picks an available one (returns its index) or quits
    // (returns -1). The focus is kept between calls.
    int choose(const std::vector<GameStatus> &games);

    // Hides the window while a game runs, and brings it back afterwards.
    void suspend();
    void resume();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rsdkair
