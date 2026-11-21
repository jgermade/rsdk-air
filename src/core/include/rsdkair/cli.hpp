// Command line:
//
//   rsdk-air                          show the game picker
//   rsdk-air --game <id> [-- args…]   run one game (what the picker starts as a child)
//   rsdk-air --list                   print the known games and whether they are available
//   rsdk-air --version | --help
//
//   --data-root <dir>                 use <dir> instead of the default data folder
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rsdkair {

struct Options {
    enum class Mode { Picker, Game, List, Version, Help };

    Mode mode = Mode::Picker;
    std::string game;
    std::optional<std::filesystem::path> data_root;
    std::vector<std::string> extra_args;  // after "--", passed to the engine
    std::string error;                    // non-empty if the command line is invalid
};

Options parse_args(const std::vector<std::string> &args);  // args without argv[0]
Options parse_args(int argc, char **argv);

std::string usage();

} // namespace rsdkair
