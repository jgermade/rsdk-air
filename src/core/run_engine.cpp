#include <rsdkair/run_engine.hpp>

#include <rsdkair/engine.h>
#include <rsdkair/module.hpp>
#include <rsdkair/paths.hpp>

#include <cstring>
#include <system_error>

namespace rsdkair {

int run_engine(const EngineRun &run, std::string &error)
{
    std::unique_ptr<Module> module = Module::open(run.module, error);
    if (!module)
        return kExitModuleMissing;

    auto abi_version = module->function<RsdkAirEngineAbiVersionFn>("rsdkair_engine_abi_version");
    auto games = module->function<RsdkAirEngineGamesFn>("rsdkair_engine_games");
    auto engine_main = module->function<RsdkAirEngineMainFn>("rsdkair_engine_main");
    if (!abi_version || !games || !engine_main) {
        error = path_to_utf8(run.module) + " is not an rsdk-air engine module (missing exports)";
        return kExitModuleInvalid;
    }
    if (abi_version() != RSDKAIR_ABI_VERSION) {
        error = path_to_utf8(run.module) + " was built for engine ABI " + std::to_string(abi_version()) +
                ", this launcher uses " + std::to_string(RSDKAIR_ABI_VERSION);
        return kExitModuleInvalid;
    }

    bool supported = false;
    for (const RsdkAirGameInfo *const *game = games(); game && *game; ++game) {
        if ((*game)->id && run.game_id == (*game)->id)
            supported = true;
    }
    if (!supported) {
        error = path_to_utf8(run.module) + " cannot run game '" + run.game_id + "'";
        return kExitModuleInvalid;
    }

    std::error_code ec;
    std::filesystem::create_directories(run.save_dir, ec);
    if (ec) {
        error = "cannot create " + path_to_utf8(run.save_dir) + ": " + ec.message();
        return kExitSaveDirFailed;
    }

    const std::string data_dir = path_to_utf8(run.data_dir);
    const std::string save_dir = path_to_utf8(run.save_dir);
    std::vector<std::string> storage = run.extra_args;
    std::vector<char *> argv;
    for (std::string &arg : storage)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    RsdkAirEngineArgs args{};
    args.abi_version = RSDKAIR_ABI_VERSION;
    args.game_id = run.game_id.c_str();
    args.data_dir = data_dir.c_str();
    args.save_dir = save_dir.c_str();
    args.argc = static_cast<int>(storage.size());
    args.argv = argv.data();
    args.host = run.host;
    return engine_main(&args);
}

std::string describe_exit_code(int code)
{
    switch (code) {
    case kExitOk: return "the game exited normally";
    case kExitUsage: return "invalid command line";
    case kExitGameUnavailable: return "the game's data is missing or invalid";
    case kExitModuleMissing: return "the engine module could not be loaded";
    case kExitModuleInvalid: return "the engine module is invalid or does not support this game";
    case kExitSaveDirFailed: return "the save folder could not be created";
    case RSDKAIR_EXIT_BAD_ARGS: return "the engine rejected its arguments";
    case RSDKAIR_EXIT_ENGINE_FAILED: return "the engine could not start (game data, graphics or audio)";
    default: return "the game exited with code " + std::to_string(code);
    }
}

} // namespace rsdkair
