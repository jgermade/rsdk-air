// Paths: UTF-8 conversion, the running executable and where engine modules live.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace rsdkair {

std::filesystem::path path_from_utf8(const std::string &utf8);
std::string path_to_utf8(const std::filesystem::path &path);

// Absolute path of the running executable (empty if unknown, and on Android).
std::filesystem::path self_executable();

// File name of an engine module: "rsdkv4.dll" on Windows, "rsdkv4.so" on Linux and macOS, or
// "librsdkv4.so" on Android.
std::string module_file_name(std::string_view module);

// Where to load a module from: <exe dir>/engines/<file> on desktop; on Android just the
// file name, which the dynamic linker finds in the APK's library directory.
std::filesystem::path module_path(std::string_view module);

} // namespace rsdkair
