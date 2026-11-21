#include <rsdkair/detect.hpp>

#include <rsdkair/engines.hpp>
#include <rsdkair/paths.hpp>
#include <rsdkair/rom.hpp>

#include <algorithm>
#include <system_error>

namespace rsdkair {

namespace fs = std::filesystem;

fs::path game_data_dir(const fs::path &root, const GameSpec &game)
{
    return root / "games" / path_from_utf8(std::string(game.id));
}

fs::path game_save_dir(const fs::path &root, const GameSpec &game)
{
    return root / "saves" / path_from_utf8(std::string(game.id));
}

std::string probe_engine(std::string_view module)
{
    if (std::find(kBuiltEngines.begin(), kBuiltEngines.end(), module) == kBuiltEngines.end())
        return "this build of rsdk-air does not include the " + std::string(module) + " engine";
#if !defined(__ANDROID__)
    // On Android the module is a library inside the APK, found by name only.
    const fs::path path = module_path(module);
    std::error_code ec;
    if (!fs::is_regular_file(path, ec))
        return "missing engine module " + path_to_utf8(path);
#endif
    return {};
}

GameStatus detect_game(const fs::path &root, const GameSpec &game, const EngineProbe &probe)
{
    GameStatus status;
    status.spec = &game;
    status.data_dir = game_data_dir(root, game);
    status.save_dir = game_save_dir(root, game);

    status.reason = probe(game.module);
    if (!status.reason.empty())
        return status;
    status.engine_found = true;

    for (std::string_view file : game.required) {
        std::error_code ec;
        const fs::path path = status.data_dir / path_from_utf8(std::string(file));
        if (!fs::is_regular_file(path, ec)) {
            status.reason = "missing " + path_to_utf8(path);
            return status;
        }
    }

    if (game.validator == Validator::S3kLockOn) {
        const Check rom = check_s3k_rom_file(status.data_dir / path_from_utf8(std::string(game.required.front())));
        if (!rom.ok) {
            status.reason = rom.reason;
            return status;
        }
    }

    status.available = true;
    return status;
}

std::vector<GameStatus> detect_games(const fs::path &root, const EngineProbe &probe)
{
    std::vector<GameStatus> result;
    for (const GameSpec &game : catalog())
        result.push_back(detect_game(root, game, probe));
    return result;
}

} // namespace rsdkair
