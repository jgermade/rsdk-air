/*
 * rsdk-air engine ABI.
 *
 * Every engine module (rsdkv3, rsdkv4, rsdkv5, sonic3air) is a shared library built
 * with hidden symbols that exports only the three functions below. The launcher loads
 * exactly one module per process (see ROADMAP/2026-11-20/architecture.md).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef RSDKAIR_ENGINE_H
#define RSDKAIR_ENGINE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Bumped on any incompatible change to the structs or functions below.
 * 2: RsdkAirEngineArgs::host, for the in-game menu (RsdkAirHost, RsdkAirEngineControl). */
#define RSDKAIR_ABI_VERSION 2

#if defined(_WIN32)
#define RSDKAIR_EXPORT __declspec(dllexport)
#else
#define RSDKAIR_EXPORT __attribute__((visibility("default")))
#endif

/* One game a module can run. RSDKv4 lists two (Sonic 1 and Sonic 2). */
typedef struct RsdkAirGameInfo {
    const char *id;                /* "sonic1", "sonic2", "soniccd", "mania", "s3air" */
    const char *title;             /* human-readable name */
    const char *const *required;   /* NULL-terminated, relative to the game's data dir */
} RsdkAirGameInfo;

/* ---- In-game menu (ABI 2) --------------------------------------------------------
 *
 * The launcher draws one menu over every game: when the game opens and when the player
 * pauses (Escape, Android back, Select + Start). It holds the save slots (where the
 * engine has a slot bridge), the game's own special screens, settings and "back to the
 * launcher". The module tells the host how the engine draws and what the menu may do
 * (RsdkAirEngineControl); the engine calls the host's hooks (RsdkAirHost). Everything
 * runs on the engine's thread; while the menu is open the engine is blocked inside a
 * hook, so the game is frozen. See ROADMAP/2026-11-20/game-menus.md. */

/* How the engine draws, so the host can draw over it. */
#define RSDKAIR_GFX_GL1 1          /* OpenGL 1.x / OpenGL ES 1.x context, fixed function */
#define RSDKAIR_GFX_SDL_RENDERER 2 /* an SDL_Renderer */

/* One row of the settings screen. */
#define RSDKAIR_SETTING_RANGE 1  /* an integer in [min, max], moved by step */
#define RSDKAIR_SETTING_CHOICE 2 /* an index into choices */
typedef struct RsdkAirSetting {
    const char *label;          /* "Music volume" */
    int kind;                   /* RSDKAIR_SETTING_* */
    int min, max, step;         /* RANGE */
    const char *const *choices; /* CHOICE: NULL-terminated labels */
    int value;
} RsdkAirSetting;

/* Save slots the menu shows, by their engine's layout. */
#define RSDKAIR_SLOTS_NONE 0
#define RSDKAIR_SLOTS_RSDKV4 1 /* saveRAM ints, 8 per slot (src/core saves_rsdkv4) */

/* What the module offers the menu. Any function may be NULL when unsupported. */
typedef struct RsdkAirEngineControl {
    void *window;   /* SDL_Window* */
    int gfx;        /* RSDKAIR_GFX_* */
    void *renderer; /* SDL_Renderer* for RSDKAIR_GFX_SDL_RENDERER, else NULL */

    /* Called with 1 when the menu opens over a running game, 0 when it closes. */
    void (*set_paused)(int paused);
    /* Asks the engine to end its main loop; rsdkair_engine_main then returns. */
    void (*request_exit)(void);

    /* The game's own screens (title screen, level select…), found in its data. */
    int (*screen_count)(void);
    const char *(*screen_name)(int index);
    void (*load_screen)(int index);

    /* Settings, applied and saved by the engine (in its own settings file). */
    int (*setting_count)(void);
    int (*setting_get)(int index, RsdkAirSetting *out);
    void (*setting_set)(int index, int value);

    /* Save slots (slots_kind != RSDKAIR_SLOTS_NONE). */
    int slots_kind;
    int (*read_save_ram)(int *out, int count); /* copies the engine's live save data */
    void (*start_slot)(int slot, int player);  /* slot -1 = no save; continues a saved slot */
    void (*delete_slot)(int slot);
} RsdkAirEngineControl;

/* The host's hooks, given to the module. ctx goes back as the first argument. */
typedef struct RsdkAirHost {
    void *ctx;
    /* Once the engine's window exists (again if it is re-created). `starting` is 1 when
     * the game has not run a frame yet: the host shows the menu right away, blocking,
     * as the game's start screen. */
    void (*attach)(void *ctx, const RsdkAirEngineControl *control, int starting);
    /* Every SDL_Event the engine polls, before it handles it. Nonzero: the menu took
     * it (e.g. the key that opens it) and the engine must skip it. */
    int (*event)(void *ctx, const void *sdl_event);
    /* After the engine drew a frame, just before it presents it. Opens the menu
     * (blocking) when the player asked for it. */
    void (*frame)(void *ctx);
} RsdkAirHost;

/* What the launcher passes to rsdkair_engine_main. All strings are UTF-8. */
typedef struct RsdkAirEngineArgs {
    int abi_version;      /* RSDKAIR_ABI_VERSION of the launcher */
    const char *game_id;  /* one of the ids from rsdkair_engine_games() */
    const char *data_dir; /* read-only game data, e.g. <root>/games/sonic1 */
    const char *save_dir; /* writable and already created, e.g. <root>/saves/sonic1 */
    int argc;             /* extra arguments given after "--" on the command line */
    char **argv;
    const RsdkAirHost *host; /* the in-game menu; NULL when there is none (tests, tools) */
} RsdkAirEngineArgs;

/* Exit codes rsdkair_engine_main may return besides the engine's own. */
#define RSDKAIR_EXIT_OK 0
#define RSDKAIR_EXIT_BAD_ARGS 64       /* invalid arguments or unsupported game */
#define RSDKAIR_EXIT_ENGINE_FAILED 65   /* the engine could not start (data, graphics, audio) */

typedef int (*RsdkAirEngineAbiVersionFn)(void);
typedef const RsdkAirGameInfo *const *(*RsdkAirEngineGamesFn)(void);
typedef int (*RsdkAirEngineMainFn)(const RsdkAirEngineArgs *args);

/* Returns RSDKAIR_ABI_VERSION as the module was built. */
RSDKAIR_EXPORT int rsdkair_engine_abi_version(void);

/* NULL-terminated list of the games this module can run. */
RSDKAIR_EXPORT const RsdkAirGameInfo *const *rsdkair_engine_games(void);

/* Runs the game until it exits. Returns RSDKAIR_EXIT_OK on a normal exit. */
RSDKAIR_EXPORT int rsdkair_engine_main(const RsdkAirEngineArgs *args);

#ifdef __cplusplus
}
#endif

#endif /* RSDKAIR_ENGINE_H */
