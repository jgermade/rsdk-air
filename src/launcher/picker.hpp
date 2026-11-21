// The game picker: a window with a mosaic of game covers, in the style of play.germade.
//
// Runs in the launcher process only. It never loads an engine module: it shows the
// catalog with what detect_games() found and returns the player's choice.
//
// Input (engine-specs UX.md): arrows / d-pad / left stick move the focus, A / Enter / Space
// plays, B / Escape asks whether to quit (No focused). Mouse: a click focuses a cover, a
// click on the focused cover (or on Play) plays it. Android: a tap only focuses a cover;
// the game starts from the Play button (or A / Enter).
//
// Art comes from <resources>/art/covers/<id>.jpg and fonts from <resources>/fonts/; any
// missing file falls back to neutral art and Dear ImGui's built-in font.
#pragma once

#include <rsdkair/detect.hpp>

#include <functional>
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

    static constexpr int kQuit = -1;
    static constexpr int kInstallBase = -100;   // kInstallBase - i: install game i first
    static constexpr int kImportBase = -1000;   // kImportBase - i: import game i's data (Android)

    // Shows `games` until the player picks an available one (returns its index), asks to
    // install one (returns kInstallBase - index; Sonic 3 A.I.R. on desktop), asks to import one's
    // data (returns kImportBase - index; Android) or quits
    // (returns kQuit). The focus is kept between calls.
    int choose(const std::vector<GameStatus> &games);

    // Engine arguments for the game choose() just returned (e.g. "--slot 1 --player 0"
    // from the save-slot screen of Sonic 1/2); empty for the game's own menus.
    std::vector<std::string> launch_args() const;

    // Shows a progress dialog over the picker until done() is true. status() gives the
    // line of text, progress() a fraction (negative: unknown). Polled every frame.
    void busy(const std::string &title, const std::function<std::string()> &status,
              const std::function<float()> &progress, const std::function<bool()> &done);

    // Hides the window while a game runs, and brings it back afterwards.
    void suspend();
    void resume();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace rsdkair
