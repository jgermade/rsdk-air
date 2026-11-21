#include <rsdkair/detect.hpp>

#include <rsdkair/paths.hpp>
#include <rsdkair/rom.hpp>

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

GameStatus detect_game(const fs::path &root, const GameSpec &game)
{
    GameStatus status;
    status.spec = &game;
    status.data_dir = game_data_dir(root, game);
    status.save_dir = game_save_dir(root, game);

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

std::vector<GameStatus> detect_games(const fs::path &root)
{
    std::vector<GameStatus> result;
    for (const GameSpec &game : catalog())
        result.push_back(detect_game(root, game));
    return result;
}

} // namespace rsdkair
