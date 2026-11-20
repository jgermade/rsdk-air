/*
 * Test-only engine module. It implements the ABI without running a game: it writes what
 * it received to <save_dir>/dummy-run.txt and returns the exit code asked for in its
 * extra arguments ("--exit <n>"), so the loader tests can check the whole round trip.
 *
 * SPDX-License-Identifier: MIT
 */
#include <rsdkair/engine.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *const dummy_required[] = {"dummy.dat", NULL};
static const RsdkAirGameInfo dummy_game = {"dummy", "Dummy Game", dummy_required};
static const RsdkAirGameInfo *const dummy_games[] = {&dummy_game, NULL};

/* Not exported: the tests check that hidden symbols cannot be resolved. */
int rsdkair_dummy_hidden_helper(void) { return 42; }

RSDKAIR_EXPORT int rsdkair_engine_abi_version(void) { return RSDKAIR_ABI_VERSION; }

RSDKAIR_EXPORT const RsdkAirGameInfo *const *rsdkair_engine_games(void) { return dummy_games; }

RSDKAIR_EXPORT int rsdkair_engine_main(const RsdkAirEngineArgs *args)
{
    if (!args || args->abi_version != RSDKAIR_ABI_VERSION || !args->game_id || strcmp(args->game_id, "dummy") != 0)
        return RSDKAIR_EXIT_BAD_ARGS;

    char path[4096];
    snprintf(path, sizeof(path), "%s/dummy-run.txt", args->save_dir);
    FILE *out = fopen(path, "w");
    if (!out)
        return RSDKAIR_EXIT_BAD_ARGS;
    fprintf(out, "game=%s\ndata=%s\nargc=%d\n", args->game_id, args->data_dir, args->argc);
    fclose(out);

    for (int i = 0; i + 1 < args->argc; ++i) {
        if (strcmp(args->argv[i], "--exit") == 0)
            return atoi(args->argv[i + 1]);
    }
    return rsdkair_dummy_hidden_helper() == 42 ? RSDKAIR_EXIT_OK : 1;
}
