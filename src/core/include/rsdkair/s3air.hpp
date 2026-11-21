// Sonic 3 A.I.R. on desktop: the official build (github.com/Eukaryot/sonic3air releases),
// downloaded and installed by rsdk-air and run as a separate program. Nothing of it is
// built or linked into rsdk-air (it is GPL-3.0 and keeps its own SDL, data and saves).
//
//   <root>/games/s3air/Sonic_Knuckles_wSonic3.bin   the player's ROM (required)
//   <root>/games/s3air/audioremaster.bin            optional remastered audio
//   <root>/apps/sonic3air/<version>/                the installed official build
//
// Only the release pinned below is installed, and only if its SHA-256 matches.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace rsdkair {

struct S3airPackage {
    enum class Archive { TarGz, Zip, Dmg };
    std::string_view version;     // e.g. "26.03.28.0"
    std::string_view url;         // official release asset
    std::string_view sha256;      // of the downloaded file, lowercase hex
    std::uint64_t size;           // bytes, for progress
    Archive archive;
    std::string_view executable;  // relative to the install folder
    std::string_view config;      // config.json relative to the install folder ("" on macOS)
    std::string_view data;        // the game's data/ folder, relative ("" if not writable)
};

// The official build for this platform, or nullptr (Android uses the official app).
const S3airPackage *s3air_package();

std::filesystem::path s3air_install_dir(const std::filesystem::path &root);
std::filesystem::path s3air_executable(const std::filesystem::path &root);
bool s3air_installed(const std::filesystem::path &root);

// config.json text with "RomPath" set to rom (any previous "RomPath" line removed). The file
// allows // comments, so it is edited as text: the key goes right after the first '{'.
std::string s3air_set_rom_path(const std::string &config_json, const std::filesystem::path &rom);

// Progress of an install, shared with the UI thread.
struct S3airInstallState {
    enum Stage { Starting, Downloading, Verifying, Extracting, Finishing, Done };
    std::atomic<int> stage{Starting};
    std::filesystem::path download;  // where the archive is being written
    std::uint64_t expected_size = 0;
};

// Downloads, verifies and unpacks the official build into s3air_install_dir(root). Uses the
// system's curl and tar (Windows 10+, Linux, macOS) and hdiutil on macOS. Runs for a while:
// call it off the UI thread. Returns false with error set on failure (nothing half-installed
// is left behind).
bool s3air_install(const std::filesystem::path &root, S3airInstallState &state, std::string &error);

// Before each launch: points the installed game at the player's ROM, and links
// audioremaster.bin into its data folder when the player has one.
bool s3air_prepare_launch(const std::filesystem::path &root, std::string &error);

} // namespace rsdkair
