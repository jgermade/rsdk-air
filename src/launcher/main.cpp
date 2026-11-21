// rsdk-air entry point.
//
// Two roles, chosen by the command line (src/core/include/rsdkair/cli.hpp):
// - launcher: finds the game data and shows the picker. It never loads an engine module;
//   it starts a game process instead (a child process on desktop, GameActivity in the
//   ":game" process on Android).
// - game process (--game <id>): loads that game's engine module and runs it.
//
// The picker is a plain SDL message box for now; M2 replaces it with a Dear ImGui UI.

#include <rsdkair/catalog.hpp>
#include <rsdkair/cli.hpp>
#include <rsdkair/detect.hpp>
#include <rsdkair/engine.h>
#include <rsdkair/paths.hpp>
#include <rsdkair/process.hpp>
#include <rsdkair/run_engine.hpp>
#include <rsdkair/version.hpp>

#include <SDL.h>

#include <cstdio>
#include <string>
#include <vector>

#if defined(__ANDROID__)
#include <jni.h>
#endif

namespace fs = std::filesystem;
using namespace rsdkair;

namespace {

constexpr const char *kAppName = "rsdk-air";

fs::path default_data_root()
{
    char *pref = SDL_GetPrefPath("", kAppName);
    if (!pref)
        return fs::current_path();
    fs::path root = path_from_utf8(pref);
    SDL_free(pref);
    return root;
}

void show_error(const std::string &title, const std::string &message)
{
    std::fprintf(stderr, "%s: %s\n", title.c_str(), message.c_str());
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, title.c_str(), message.c_str(), nullptr);
}

int list_games(const fs::path &root)
{
    std::printf("Data folder: %s\n\n", path_to_utf8(root).c_str());
    for (const GameStatus &status : detect_games(root)) {
        std::printf("%-8s %-22s %s\n", std::string(status.spec->id).c_str(), std::string(status.spec->title).c_str(),
                    status.available ? "available" : ("not available: " + status.reason).c_str());
    }
    return kExitOk;
}

int run_game(const Options &options, const fs::path &root)
{
    const GameSpec *game = find_game(options.game);
    if (!game) {
        show_error(kAppName, "Unknown game: " + options.game);
        return kExitGameUnavailable;
    }
    const GameStatus status = detect_game(root, *game);
    if (!status.available) {
        show_error(std::string(game->title), status.reason);
        return kExitGameUnavailable;
    }

    EngineRun run;
    run.module = module_path(game->module);
    run.game_id = std::string(game->id);
    run.data_dir = status.data_dir;
    run.save_dir = status.save_dir;
    run.extra_args = options.extra_args;

    std::string error;
    const int code = run_engine(run, error);
    if (!error.empty())
        show_error(std::string(game->title), error);
    return code;
}

#if defined(__ANDROID__)
// Asks LauncherActivity (android/) to start GameActivity in the ":game" process.
bool start_game_process(const GameSpec &game, const fs::path &root, std::string &error)
{
    auto *env = static_cast<JNIEnv *>(SDL_AndroidGetJNIEnv());
    auto activity = static_cast<jobject>(SDL_AndroidGetActivity());
    if (!env || !activity) {
        error = "no Android activity";
        return false;
    }
    jclass cls = env->GetObjectClass(activity);
    jmethodID start = env->GetMethodID(cls, "startGame", "(Ljava/lang/String;Ljava/lang/String;)V");
    if (start) {
        jstring id = env->NewStringUTF(std::string(game.id).c_str());
        jstring data_root = env->NewStringUTF(path_to_utf8(root).c_str());
        env->CallVoidMethod(activity, start, id, data_root);
        env->DeleteLocalRef(id);
        env->DeleteLocalRef(data_root);
    }
    const bool failed = !start || env->ExceptionCheck();
    if (env->ExceptionCheck())
        env->ExceptionClear();
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    if (failed) {
        error = "LauncherActivity.startGame failed";
        return false;
    }

    // Wait until the player comes back from the game.
    SDL_Event event;
    while (SDL_WaitEvent(&event)) {
        if (event.type == SDL_APP_DIDENTERFOREGROUND)
            return true;
        if (event.type == SDL_QUIT) {
            error.clear();
            return false;
        }
    }
    return true;
}
#else
bool start_game_process(const GameSpec &game, const fs::path &root, std::string &error)
{
    const fs::path exe = self_executable();
    if (exe.empty()) {
        error = "cannot find the rsdk-air executable";
        return false;
    }
    const int code = run_child(exe, {"--game", std::string(game.id), "--data-root", path_to_utf8(root)}, error);
    if (code < 0)
        return false;
    if (code != kExitOk) {
        // The game process already showed its own error, if it had one.
        std::fprintf(stderr, "%s: %s\n", std::string(game.title).c_str(), describe_exit_code(code).c_str());
    }
    return true;
}
#endif

int run_picker(const fs::path &root)
{
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    for (;;) {
        const std::vector<GameStatus> games = detect_games(root);
        std::vector<const GameStatus *> available;
        for (const GameStatus &status : games) {
            if (status.available)
                available.push_back(&status);
        }

        std::vector<SDL_MessageBoxButtonData> buttons;
        std::vector<std::string> labels;
        labels.reserve(available.size());
        for (std::size_t i = 0; i < available.size(); ++i) {
            labels.emplace_back(available[i]->spec->title);
            buttons.push_back({0, static_cast<int>(i), labels.back().c_str()});
        }
        const int retry = -2;
        const int quit = -1;
        if (available.empty())
            buttons.push_back({SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, retry, "Look again"});
        buttons.push_back({SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, quit, "Quit"});

        std::string message;
        if (available.empty()) {
            message = "No games found. Put your own game files here:\n\n";
            for (const GameStatus &status : games) {
                message += "  " + std::string(status.spec->title) + ": " + path_to_utf8(status.data_dir) + "/" +
                           std::string(status.spec->required.front()) + "\n";
            }
        } else {
            message = "Which game do you want to play?";
        }

        const std::string title = std::string(kAppName) + " " + kVersion;
        SDL_MessageBoxData box{};
        box.flags = SDL_MESSAGEBOX_INFORMATION;
        box.title = title.c_str();
        box.message = message.c_str();
        box.numbuttons = static_cast<int>(buttons.size());
        box.buttons = buttons.data();

        int choice = quit;
        if (SDL_ShowMessageBox(&box, &choice) != 0) {
            std::fprintf(stderr, "cannot show the picker: %s\n", SDL_GetError());
            break;
        }
        if (choice == quit)
            break;
        if (choice == retry)
            continue;

        std::string error;
        const GameSpec &game = *available[static_cast<std::size_t>(choice)]->spec;
        if (!start_game_process(game, root, error)) {
            if (error.empty())
                break;  // the app is quitting
            show_error(std::string(game.title), error);
        }
    }

    SDL_Quit();
    return kExitOk;
}

} // namespace

int main(int argc, char **argv)
{
    const Options options = parse_args(argc, argv);
    if (!options.error.empty()) {
        std::fprintf(stderr, "%s\n\n%s", options.error.c_str(), usage().c_str());
        return kExitUsage;
    }

    switch (options.mode) {
    case Options::Mode::Help: std::printf("%s", usage().c_str()); return kExitOk;
    case Options::Mode::Version:
        std::printf("%s %s (engine ABI %d, Plus %s)\n", kAppName, kVersion, RSDKAIR_ABI_VERSION,
                    kPlusEnabled ? "on" : "off");
        return kExitOk;
    default: break;
    }

    const fs::path root = options.data_root ? *options.data_root : default_data_root();
    switch (options.mode) {
    case Options::Mode::List: return list_games(root);
    case Options::Mode::Game: return run_game(options, root);
    default: return run_picker(root);
    }
}
