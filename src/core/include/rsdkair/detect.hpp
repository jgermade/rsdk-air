// Finding the player's game data.
//
//   <root>/games/<id>/<required files>   read-only game data
//   <root>/saves/<id>/                   written by the engine
#pragma once

#include <rsdkair/catalog.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace rsdkair {

struct GameStatus {
    const GameSpec *spec = nullptr;
    bool available = false;
    std::string reason;  // why it is not available (empty when available)
    std::filesystem::path data_dir;
    std::filesystem::path save_dir;
};

std::filesystem::path game_data_dir(const std::filesystem::path &root, const GameSpec &game);
std::filesystem::path game_save_dir(const std::filesystem::path &root, const GameSpec &game);

GameStatus detect_game(const std::filesystem::path &root, const GameSpec &game);

// One entry per catalog() game, in the same order.
std::vector<GameStatus> detect_games(const std::filesystem::path &root);

} // namespace rsdkair
