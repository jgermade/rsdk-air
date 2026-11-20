// Minimal edits to INI text (the format RSDK's IniParser reads: "[Section]" and
// "Key=Value" lines). Everything else in the text is kept as it was.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace rsdkair {

// Sets section/key to value: replaces the existing line (keys compare case-insensitively),
// or adds it at the end of the section, or adds the section at the end of the text.
std::string ini_set(std::string_view text, std::string_view section, std::string_view key, std::string_view value);

// The value of section/key, or empty if absent.
std::string ini_get(std::string_view text, std::string_view section, std::string_view key);

// Reads path (empty text if missing), applies ini_set, writes it back. False on I/O error.
bool ini_set_file(const std::filesystem::path &path, std::string_view section, std::string_view key,
                  std::string_view value);

} // namespace rsdkair
