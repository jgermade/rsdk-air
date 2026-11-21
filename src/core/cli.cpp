#include <rsdkair/cli.hpp>

#include <rsdkair/paths.hpp>

namespace rsdkair {

Options parse_args(const std::vector<std::string> &args)
{
    Options options;
    bool mode_set = false;
    auto set_mode = [&](Options::Mode mode, const std::string &flag) {
        if (mode_set && options.mode != mode)
            options.error = flag + " cannot be combined with another mode";
        options.mode = mode;
        mode_set = true;
    };

    for (std::size_t i = 0; i < args.size() && options.error.empty(); ++i) {
        const std::string &arg = args[i];
        if (arg == "--") {
            options.extra_args.assign(args.begin() + static_cast<std::ptrdiff_t>(i) + 1, args.end());
            break;
        } else if (arg == "--game") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                options.error = "--game needs a game id";
                break;
            }
            set_mode(Options::Mode::Game, arg);
            options.game = args[++i];
        } else if (arg == "--data-root") {
            if (i + 1 >= args.size() || args[i + 1].empty()) {
                options.error = "--data-root needs a folder";
                break;
            }
            options.data_root = path_from_utf8(args[++i]);
        } else if (arg == "--list") {
            set_mode(Options::Mode::List, arg);
        } else if (arg == "--version") {
            set_mode(Options::Mode::Version, arg);
        } else if (arg == "--help" || arg == "-h") {
            set_mode(Options::Mode::Help, arg);
        } else if (arg.rfind("-psn_", 0) == 0) {
            // Added by macOS when an .app is opened from Finder on old systems; ignore.
        } else {
            options.error = "unknown argument: " + arg;
        }
    }

    if (options.error.empty() && !options.extra_args.empty() && options.mode != Options::Mode::Game)
        options.error = "arguments after -- are only allowed with --game";
    return options;
}

Options parse_args(int argc, char **argv)
{
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.emplace_back(argv[i] ? argv[i] : "");
    return parse_args(args);
}

std::string usage()
{
    return "Usage:\n"
           "  rsdk-air                          show the game picker\n"
           "  rsdk-air --game <id> [-- args...] run one game\n"
           "  rsdk-air --list                   list games and whether their data was found\n"
           "  rsdk-air --version\n"
           "\n"
           "Options:\n"
           "  --data-root <dir>                 data folder (default: the user data folder)\n";
}

} // namespace rsdkair
