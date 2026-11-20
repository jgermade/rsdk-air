// ROM checks. Same rules as sonic3air-wasm (and the official Sonic 3 A.I.R. web page).
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>

namespace rsdkair {

struct Check {
    bool ok = false;
    std::string reason;  // empty when ok; otherwise words a player can act on
};

inline constexpr std::size_t kS3kRomSize = 4 * 1024 * 1024;
inline constexpr std::size_t kS3kSerialOffset = 0x180;
inline constexpr std::size_t kS3kSerialLength = 14;

// size: the whole file's size; header: at least the first 0x190 bytes (or the whole file).
Check check_s3k_rom(std::uintmax_t size, std::span<const std::uint8_t> header);

// Reads only the header of the file at path.
Check check_s3k_rom_file(const std::filesystem::path &path);

} // namespace rsdkair
