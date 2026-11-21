// Finding the player's game data, and whether this build can run the game.
//
//   <root>/games/<id>/<required files>   read-only game data
//   <root>/saves/<id>/                   written by the engine
#pragma once

#include <rsdkair/catalog.hpp>

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace rsdkair {

struct GameStatus {
    const GameSpec *spec = nullptr;
    bool available = false;     // engine found and data found
    bool engine_found = false;  // this build can run the game (its data may still be missing)
    std::string reason;         // why it is not available (empty when available)
    std::filesystem::path data_dir;
    std::filesystem::path save_dir;
};

std::filesystem::path game_data_dir(const std::filesystem::path &root, const GameSpec &game);
std::filesystem::path game_save_dir(const std::filesystem::path &root, const GameSpec &game);

// Empty if the engine module can be used, otherwise why not.
using EngineProbe = std::function<std::string(std::string_view module)>;

// The default probe: the module must be one this build includes (kBuiltEngines) and, on
// desktop, its file must exist next to the executable. Never loads the module.
std::string probe_engine(std::string_view module);

// The engine is checked first: there is no point asking for data the build cannot run.
GameStatus detect_game(const std::filesystem::path &root, const GameSpec &game,
                       const EngineProbe &probe = probe_engine);

// One entry per catalog() game, in the same order.
std::vector<GameStatus> detect_games(const std::filesystem::path &root, const EngineProbe &probe = probe_engine);

} // namespace rsdkair
