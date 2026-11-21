#include <rsdkair/s3air.hpp>

#include <rsdkair/catalog.hpp>
#include <rsdkair/detect.hpp>
#include <rsdkair/paths.hpp>
#include <rsdkair/process.hpp>
#include <rsdkair/sha256.hpp>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <system_error>

namespace rsdkair {

namespace fs = std::filesystem;

namespace {

// Stable Update v26.03.28.0 (2026-03-28). To update: new version, URLs, sizes and the
// SHA-256 of each file, checked against the release page.
#if defined(_WIN32)
constexpr S3airPackage kPackage = {
    "26.03.28.0",
    "https://github.com/Eukaryot/sonic3air/releases/download/v26.03.28.0-stable/sonic3air_game.zip",
    "f9b2151aafaa167b95f5f27c4bb5c7b53e694c54df3055369aad7f21f4538fbf",
    16318469,
    S3airPackage::Archive::Zip,
    "sonic3air_game/Sonic3AIR.exe",
    "sonic3air_game/config.json",
    "sonic3air_game/data",
};
#define RSDKAIR_HAS_S3AIR_PACKAGE 1
#elif defined(__APPLE__)
constexpr S3airPackage kPackage = {
    "26.03.28.0",
    "https://github.com/Eukaryot/sonic3air/releases/download/v26.03.28.0-stable/sonic3air_mac_260328.dmg",
    "3631938e71df8c612bc9f5dc1a9b3ea12dce63d33f8160d122b6bea8d03fddaf",
    24326656,
    S3airPackage::Archive::Dmg,
    "Sonic 3 AIR.app/Contents/MacOS/sonic3air",
    "",  // the app reads its ROM path from its settings.json instead
    "",  // inside the signed app bundle: left alone
};
#define RSDKAIR_HAS_S3AIR_PACKAGE 1
#elif defined(__linux__) && !defined(__ANDROID__) && defined(__x86_64__)
constexpr S3airPackage kPackage = {
    "26.03.28.0",
    "https://github.com/Eukaryot/sonic3air/releases/download/v26.03.28.0-stable/sonic3air_game.tar.gz",
    "b8e268e09eb0bd8241f9e1b75be5b1ee42b8e7f1dbcea1181e54068fe5a8d7b0",
    19877172,
    S3airPackage::Archive::TarGz,
    "sonic3air_game/sonic3air_linux",
    "sonic3air_game/config.json",
    "sonic3air_game/data",
};
#define RSDKAIR_HAS_S3AIR_PACKAGE 1
#endif

std::string json_escape(const std::string &text)
{
    std::string out;
    for (char c : text) {
        if (c == '\\' || c == '"')
            out += '\\';
        out += c;
    }
    return out;
}

bool read_text(const fs::path &path, std::string &text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return false;
    std::stringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

bool write_text(const fs::path &path, const std::string &text)
{
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    return static_cast<bool>(out);
}

bool tool(const char *name, const std::vector<std::string> &args, std::string &error)
{
    const int code = run_child(name, args, error, {}, true);
    if (code < 0)
        return false;
    if (code != 0) {
        error = std::string(name) + " failed (exit code " + std::to_string(code) + ")";
        return false;
    }
    return true;
}

const GameSpec &s3air_game() { return *find_game("s3air"); }

} // namespace

const S3airPackage *s3air_package()
{
#if defined(RSDKAIR_HAS_S3AIR_PACKAGE)
    return &kPackage;
#else
    return nullptr;
#endif
}

fs::path s3air_install_dir(const fs::path &root)
{
    const S3airPackage *package = s3air_package();
    return root / "apps" / "sonic3air" / (package ? std::string(package->version) : std::string("none"));
}

fs::path s3air_executable(const fs::path &root)
{
    const S3airPackage *package = s3air_package();
    if (!package)
        return {};
    return s3air_install_dir(root) / path_from_utf8(std::string(package->executable));
}

bool s3air_installed(const fs::path &root)
{
    std::error_code ec;
    const fs::path exe = s3air_executable(root);
    return !exe.empty() && fs::is_regular_file(exe, ec);
}

std::string s3air_set_rom_path(const std::string &config_json, const fs::path &rom)
{
    std::string text;
    std::istringstream lines(config_json);
    std::string line;
    while (std::getline(lines, line)) {
        if (line.find("\"RomPath\"") == std::string::npos)
            text += line + "\n";
    }
    const std::string entry = "\n\t\"RomPath\": \"" + json_escape(path_to_utf8(rom)) + "\",";
    const std::size_t brace = text.find('{');
    if (brace == std::string::npos)
        return "{" + entry + "\n}\n";
    return text.insert(brace + 1, entry);
}

bool s3air_install(const fs::path &root, S3airInstallState &state, std::string &error)
{
    const S3airPackage *package = s3air_package();
    if (!package) {
        error = "there is no official Sonic 3 A.I.R. build for this system";
        return false;
    }
    std::error_code ec;
    const fs::path final_dir = s3air_install_dir(root);
    const fs::path work = final_dir.parent_path() / (std::string(package->version) + ".partial");
    fs::remove_all(work, ec);
    fs::create_directories(work, ec);
    if (ec) {
        error = "cannot create " + path_to_utf8(work) + ": " + ec.message();
        return false;
    }
    auto fail = [&](const std::string &message) {
        error = message;
        fs::remove_all(work, ec);
        return false;
    };

    const char *ext = package->archive == S3airPackage::Archive::Zip   ? ".zip"
                      : package->archive == S3airPackage::Archive::Dmg ? ".dmg"
                                                                       : ".tar.gz";
    const fs::path archive = work / (std::string("download") + ext);
    state.download = archive;
    state.expected_size = package->size;

    state.stage = S3airInstallState::Downloading;
    if (!tool("curl", {"-fsSL", "--retry", "3", "-o", path_to_utf8(archive), std::string(package->url)}, error))
        return fail("download failed: " + error);

    state.stage = S3airInstallState::Verifying;
    const std::string hash = sha256_file(archive);
    if (hash != package->sha256)
        return fail("the downloaded file does not match the expected SHA-256 (got " + hash + ")");

    state.stage = S3airInstallState::Extracting;
    const fs::path files = work / "files";
    fs::create_directories(files, ec);
    if (package->archive == S3airPackage::Archive::Dmg) {
        const fs::path mount = work / "mount";
        fs::create_directories(mount, ec);
        if (!tool("hdiutil", {"attach", "-nobrowse", "-readonly", "-noautoopen", "-mountpoint", path_to_utf8(mount),
                              path_to_utf8(archive)},
                  error))
            return fail("cannot open the disk image: " + error);
        const std::string app = path_to_utf8(fs::path(std::string(package->executable)).begin()->string());
        const bool copied = tool("cp", {"-R", path_to_utf8(mount / app), path_to_utf8(files)}, error);
        std::string detach_error;
        tool("hdiutil", {"detach", path_to_utf8(mount)}, detach_error);
        if (!copied)
            return fail("cannot copy the app: " + error);
    } else {
        // tar handles .tar.gz everywhere, and .zip too on Windows 10+ (bsdtar).
        if (!tool("tar", {"-xf", path_to_utf8(archive), "-C", path_to_utf8(files)}, error))
            return fail("cannot unpack: " + error);
    }
    if (!fs::is_regular_file(files / path_from_utf8(std::string(package->executable)), ec))
        return fail("the package does not contain " + std::string(package->executable));

    state.stage = S3airInstallState::Finishing;
    fs::remove_all(final_dir, ec);
    fs::rename(files, final_dir, ec);
    if (ec)
        return fail("cannot move the game into place: " + ec.message());
    fs::remove_all(work, ec);
    state.stage = S3airInstallState::Done;
    return true;
}

bool s3air_prepare_launch(const fs::path &root, std::string &error)
{
    const S3airPackage *package = s3air_package();
    if (!package || !s3air_installed(root)) {
        error = "Sonic 3 A.I.R. is not installed";
        return false;
    }
    const GameSpec &game = s3air_game();
    const fs::path data_dir = game_data_dir(root, game);
    const fs::path rom = fs::absolute(data_dir / path_from_utf8(std::string(game.required.front())));
    const fs::path install = s3air_install_dir(root);
    std::error_code ec;

    if (!package->config.empty()) {
        const fs::path config = install / path_from_utf8(std::string(package->config));
        std::string text;
        read_text(config, text);  // missing is fine: written from scratch
        if (!write_text(config, s3air_set_rom_path(text, rom))) {
            error = "cannot write " + path_to_utf8(config);
            return false;
        }
    }
#if defined(__APPLE__)
    else if (const char *home = std::getenv("HOME")) {
        // The macOS app keeps the ROM path in its settings.json. Only a first run is
        // seeded; after that the app's own choice stays.
        const fs::path dir = fs::path(home) / "Library/Application Support/Sonic3AIR";
        const fs::path settings = dir / "settings.json";
        if (!fs::exists(settings, ec)) {
            fs::create_directories(dir, ec);
            write_text(settings, "{\n\t\"RomPath\": \"" + json_escape(path_to_utf8(rom)) + "\"\n}\n");
        }
    }
#endif

    // Remastered audio: linked (or copied) into the game's data folder when present.
    const fs::path remaster = data_dir / "audioremaster.bin";
    if (!package->data.empty() && fs::is_regular_file(remaster, ec)) {
        const fs::path target = install / path_from_utf8(std::string(package->data)) / "audioremaster.bin";
        if (!fs::exists(target, ec) || fs::file_size(target, ec) != fs::file_size(remaster, ec)) {
            fs::remove(target, ec);
            fs::create_hard_link(remaster, target, ec);
            if (ec) {
                ec.clear();
                fs::copy_file(remaster, target, fs::copy_options::overwrite_existing, ec);
                if (ec) {
                    error = "cannot place audioremaster.bin: " + ec.message();
                    return false;
                }
            }
        }
    }
    return true;
}

} // namespace rsdkair
