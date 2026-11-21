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

/* Bumped on any incompatible change to the structs or functions below. */
#define RSDKAIR_ABI_VERSION 1

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

/* What the launcher passes to rsdkair_engine_main. All strings are UTF-8. */
typedef struct RsdkAirEngineArgs {
    int abi_version;      /* RSDKAIR_ABI_VERSION of the launcher */
    const char *game_id;  /* one of the ids from rsdkair_engine_games() */
    const char *data_dir; /* read-only game data, e.g. <root>/games/sonic1 */
    const char *save_dir; /* writable and already created, e.g. <root>/saves/sonic1 */
    int argc;             /* extra arguments given after "--" on the command line */
    char **argv;
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
