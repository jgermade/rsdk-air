// The engine side of the in-game menu (engine ABI 2, include/rsdkair/engine.h), shared by
// the RSDK modules. A module fills `control` with what its engine can do and calls:
//   - attach(window, true) before the engine's main loop, to show the start screen;
//   - event(e) for every SDL_Event the engine polls (patched into its event loop);
//   - frame(window) after each frame is drawn, before it is presented (patched in too).
// The host is the launcher's GameMenu (src/launcher/game_menu.cpp); without one (tests,
// tools) everything here does nothing.
//
// SPDX-License-Identifier: MIT
#pragma once

#include <rsdkair/engine.h>

namespace rsdkair_menu {

struct Glue {
    const RsdkAirHost *host = nullptr;
    RsdkAirEngineControl control{};
    bool attached = false;

    void attach(void *window, bool starting)
    {
        if (!host || !host->attach || !window)
            return;
        control.window = window;
        attached = true;
        host->attach(host->ctx, &control, starting ? 1 : 0);
    }

    int event(const void *sdl_event) const
    {
        return host && attached && host->event ? host->event(host->ctx, sdl_event) : 0;
    }

    // The game's own pause (button, key): open this menu instead.
    void open() const
    {
        if (host && attached && host->open)
            host->open(host->ctx);
    }

    void frame(void *window)
    {
        if (!host)
            return;
        if (!attached || control.window != window)
            attach(window, false);  // also when the engine re-created its window
        if (host->frame)
            host->frame(host->ctx);
    }
};

} // namespace rsdkair_menu
