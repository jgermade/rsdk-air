// Starting a child process and waiting for it (desktop only).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace rsdkair {

// Runs exe with args (UTF-8), waits, and returns its exit code. Returns -1 and sets error
// if it could not be started, or if it was killed by a signal.
//   cwd:         working directory for the child (empty: the current one)
//   search_path: exe is a bare program name looked up in PATH (system tools: curl, tar)
int run_child(const std::filesystem::path &exe, const std::vector<std::string> &args, std::string &error,
              const std::filesystem::path &cwd = {}, bool search_path = false);

// Windows command-line quoting of one argument (CommandLineToArgvW rules). Exposed for tests.
std::string quote_windows_arg(const std::string &arg);

} // namespace rsdkair
