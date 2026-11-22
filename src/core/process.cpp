#include <rsdkair/process.hpp>

#include <rsdkair/paths.hpp>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif !defined(__ANDROID__)
#include <cerrno>
#include <cstring>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace rsdkair {

std::string quote_windows_arg(const std::string &arg)
{
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos)
        return arg;

    std::string quoted = "\"";
    std::size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
            continue;
        }
        if (c == '"') {
            quoted.append(backslashes * 2 + 1, '\\');
        } else {
            quoted.append(backslashes, '\\');
        }
        backslashes = 0;
        quoted.push_back(c);
    }
    quoted.append(backslashes * 2, '\\');
    quoted.push_back('"');
    return quoted;
}

int run_child(const std::filesystem::path &exe, const std::vector<std::string> &args, std::string &error,
              const std::filesystem::path &cwd, bool search_path)
{
#if defined(__ANDROID__)
    (void)exe;
    (void)args;
    (void)cwd;
    (void)search_path;
    error = "child processes are not used on Android";
    return -1;
#elif defined(_WIN32)
    std::string command_line = quote_windows_arg(path_to_utf8(exe));
    for (const std::string &arg : args)
        command_line += " " + quote_windows_arg(arg);

    const int wide_length = MultiByteToWideChar(CP_UTF8, 0, command_line.c_str(), -1, nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(wide_length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, command_line.c_str(), -1, wide.data(), wide_length);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // rsdk-air is a GUI program: with no console of its own (started from Explorer), a
    // console child would open a console window; CREATE_NO_WINDOW keeps it hidden. With a
    // console (started from a terminal) the child shares it. GUI children ignore the flag.
    const DWORD flags = GetConsoleWindow() ? 0 : CREATE_NO_WINDOW;
    // Without an application name, CreateProcessW finds the program like a shell would.
    if (!CreateProcessW(search_path ? nullptr : exe.c_str(), wide.data(), nullptr, nullptr, FALSE, flags, nullptr,
                        cwd.empty() ? nullptr : cwd.c_str(), &startup, &process)) {
        error = "cannot start " + path_to_utf8(exe) + " (error " + std::to_string(GetLastError()) + ")";
        return -1;
    }
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
#else
    const std::string exe_utf8 = path_to_utf8(exe);
    std::vector<std::string> storage;
    storage.push_back(exe_utf8);
    storage.insert(storage.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (std::string &arg : storage)
        argv.push_back(arg.data());
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    const std::string cwd_utf8 = path_to_utf8(cwd);
    if (!cwd.empty())
        posix_spawn_file_actions_addchdir_np(&actions, cwd_utf8.c_str());  // glibc 2.29+, macOS 10.15+
    pid_t pid = 0;
    const int spawn_error = search_path
        ? posix_spawnp(&pid, exe_utf8.c_str(), &actions, nullptr, argv.data(), environ)
        : posix_spawn(&pid, exe_utf8.c_str(), &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (spawn_error != 0) {
        error = "cannot start " + exe_utf8 + ": " + std::strerror(spawn_error);
        return -1;
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            error = std::string("waitpid failed: ") + std::strerror(errno);
            return -1;
        }
    }
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    error = exe_utf8 + " was terminated by signal " + std::to_string(WTERMSIG(status));
    return -1;
#endif
}

} // namespace rsdkair
