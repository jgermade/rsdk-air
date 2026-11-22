// Running one game in the current process: load its module and hand over control.
// Only ever called in a game process (`rsdk-air --game <id>`), never in the launcher.
#pragma once

#include <rsdkair/engine.h>

#include <filesystem>
#include <string>
#include <vector>

namespace rsdkair {

// Exit codes of a game process, besides the engine's own return value.
enum ExitCode : int {
    kExitOk = 0,
    kExitUsage = 2,
    kExitGameUnavailable = 3,   // unknown game, or its data is missing or invalid
    kExitModuleMissing = 4,     // the engine module cannot be loaded
    kExitModuleInvalid = 5,     // missing exports, ABI mismatch, or game not in module
    kExitSaveDirFailed = 6,     // cannot create the save directory
};

struct EngineRun {
    std::filesystem::path module;    // full path to the engine module
    std::string game_id;
    std::filesystem::path data_dir;
    std::filesystem::path save_dir;  // created if missing
    std::vector<std::string> extra_args;
    const RsdkAirHost *host = nullptr;  // the in-game menu (src/launcher/overlay.cpp), or none
};

// Returns the engine's exit code, or one of ExitCode on failure (with error set).
int run_engine(const EngineRun &run, std::string &error);

// Human-readable meaning of a game process exit code.
std::string describe_exit_code(int code);

} // namespace rsdkair
