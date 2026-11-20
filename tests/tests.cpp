// Tests for rsdkair_core. Run one case with: rsdkair_tests <name> (ctest does this).

#include <rsdkair/catalog.hpp>
#include <rsdkair/cli.hpp>
#include <rsdkair/detect.hpp>
#include <rsdkair/engine.h>
#include <rsdkair/ini.hpp>
#include <rsdkair/module.hpp>
#include <rsdkair/paths.hpp>
#include <rsdkair/process.hpp>
#include <rsdkair/rom.hpp>
#include <rsdkair/run_engine.hpp>
#include <rsdkair/saves_rsdkv4.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace rsdkair;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++g_failures;                                                                                              \
        }                                                                                                              \
    } while (0)

// A fresh, empty directory under the system temp dir.
fs::path temp_dir(const std::string &name)
{
    const fs::path dir = fs::temp_directory_path() / ("rsdkair-test-" + name);
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

void write_file(const fs::path &path, const std::vector<std::uint8_t> &bytes)
{
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}

std::string read_file(const fs::path &path)
{
    std::ifstream in(path, std::ios::binary);
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

std::vector<std::uint8_t> fake_rom(std::size_t size, const char *serial)
{
    std::vector<std::uint8_t> rom(size, 0);
    if (size >= kS3kSerialOffset + std::strlen(serial))
        std::memcpy(rom.data() + kS3kSerialOffset, serial, std::strlen(serial));
    return rom;
}

const fs::path kDummyModule = path_from_utf8(RSDKAIR_DUMMY_MODULE);
#ifdef RSDKAIR_RSDKV4_MODULE
const fs::path kRsdkv4Module = path_from_utf8(RSDKAIR_RSDKV4_MODULE);
#endif

void write_save_ram(const fs::path &file, const std::vector<std::int32_t> &ints)
{
    std::vector<std::uint8_t> bytes(rsdkv4::kSaveRamInts * 4, 0);
    for (std::size_t i = 0; i < ints.size(); ++i) {
        const auto v = static_cast<std::uint32_t>(ints[i]);
        for (int b = 0; b < 4; ++b)
            bytes[i * 4 + static_cast<std::size_t>(b)] = static_cast<std::uint8_t>(v >> (8 * b));
    }
    write_file(file, bytes);
}

// --- test cases --------------------------------------------------------------

void test_catalog()
{
    CHECK(catalog().size() == 5);
    for (const GameSpec &game : catalog()) {
        CHECK(find_game(game.id) == &game);
        CHECK(!game.required.empty());
        CHECK(!game.module.empty());
    }
    CHECK(find_game("sonic1")->module == "rsdkv4");
    CHECK(find_game("sonic2")->module == "rsdkv4");
    CHECK(find_game("soniccd")->module == "rsdkv3");
    CHECK(find_game("mania")->module == "rsdkv5");
    CHECK(find_game("s3air")->module == "sonic3air");
    CHECK(find_game("s3air")->validator == Validator::S3kLockOn);
    CHECK(find_game("nope") == nullptr);
}

void test_cli()
{
    Options o = parse_args(std::vector<std::string>{});
    CHECK(o.error.empty() && o.mode == Options::Mode::Picker && !o.data_root);

    o = parse_args({"--game", "sonic2", "--data-root", "/tmp/x", "--", "--exit", "3"});
    CHECK(o.error.empty());
    CHECK(o.mode == Options::Mode::Game && o.game == "sonic2");
    CHECK(o.data_root && *o.data_root == fs::path("/tmp/x"));
    CHECK((o.extra_args == std::vector<std::string>{"--exit", "3"}));

    CHECK(parse_args({"--list"}).mode == Options::Mode::List);
    CHECK(parse_args({"--version"}).mode == Options::Mode::Version);
    CHECK(parse_args({"-h"}).mode == Options::Mode::Help);
    CHECK(!parse_args({"--game"}).error.empty());
    CHECK(!parse_args({"--data-root"}).error.empty());
    CHECK(!parse_args({"--bogus"}).error.empty());
    CHECK(!parse_args({"--list", "--game", "sonic1"}).error.empty());
    CHECK(!parse_args({"--list", "--", "x"}).error.empty());
}

void test_rom()
{
    CHECK(check_s3k_rom(0, {}).reason.find("no ROM") != std::string::npos);

    auto good = fake_rom(kS3kRomSize, "GM MK-1563 -00");
    CHECK(check_s3k_rom(good.size(), good).ok);

    auto half = fake_rom(kS3kRomSize / 2, "GM MK-1079 -00");
    Check c = check_s3k_rom(half.size(), half);
    CHECK(!c.ok && c.reason.find("on its own") != std::string::npos);

    auto odd = fake_rom(1234, "");
    c = check_s3k_rom(odd.size(), odd);
    CHECK(!c.ok && c.reason.find("1234 bytes") != std::string::npos);

    auto wrong = fake_rom(kS3kRomSize, "GM 00001009-00");
    c = check_s3k_rom(wrong.size(), wrong);
    CHECK(!c.ok && c.reason.find("expected MK-1563") != std::string::npos);

    const fs::path dir = temp_dir("rom");
    write_file(dir / "rom.bin", good);
    CHECK(check_s3k_rom_file(dir / "rom.bin").ok);
    CHECK(!check_s3k_rom_file(dir / "missing.bin").ok);
}

void test_detect()
{
    const fs::path root = temp_dir("detect");
    for (const GameStatus &s : detect_games(root)) {
        CHECK(!s.available);
        CHECK(s.reason.find("missing") != std::string::npos);
    }

    write_file(root / "games" / "sonic1" / "Data.rsdk", {1, 2, 3});
    write_file(root / "games" / "s3air" / "Sonic_Knuckles_wSonic3.bin", fake_rom(kS3kRomSize / 2, ""));

    const auto games = detect_games(root);
    CHECK(games.size() == catalog().size());
    for (const GameStatus &s : games) {
        if (s.spec->id == "sonic1") {
            CHECK(s.available && s.reason.empty());
            CHECK(s.data_dir == root / "games" / "sonic1");
            CHECK(s.save_dir == root / "saves" / "sonic1");
        } else if (s.spec->id == "s3air") {
            CHECK(!s.available && s.reason.find("on its own") != std::string::npos);
        } else {
            CHECK(!s.available);
        }
    }

    write_file(root / "games" / "s3air" / "Sonic_Knuckles_wSonic3.bin", fake_rom(kS3kRomSize, "GM MK-1563 -00"));
    CHECK(detect_game(root, *find_game("s3air")).available);
}

void test_windows_quoting()
{
    CHECK(quote_windows_arg("plain") == "plain");
    CHECK(quote_windows_arg("") == "\"\"");
    CHECK(quote_windows_arg("with space") == "\"with space\"");
    CHECK(quote_windows_arg("say \"hi\"") == "\"say \\\"hi\\\"\"");
    CHECK(quote_windows_arg("C:\\dir with space\\") == "\"C:\\dir with space\\\\\"");
}

void test_module_hidden_symbols()
{
    std::string error;
    auto module = Module::open(kDummyModule, error);
    CHECK(module != nullptr);
    if (!module) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return;
    }
    CHECK(module->symbol("rsdkair_engine_abi_version") != nullptr);
    CHECK(module->symbol("rsdkair_engine_games") != nullptr);
    CHECK(module->symbol("rsdkair_engine_main") != nullptr);
    // Built with hidden visibility: internal symbols must not be resolvable.
    CHECK(module->symbol("rsdkair_dummy_hidden_helper") == nullptr);

    CHECK(Module::open(kDummyModule.parent_path() / "does-not-exist.so", error) == nullptr);
    CHECK(!error.empty());
}

void test_run_engine()
{
    const fs::path root = temp_dir("run");
    EngineRun run;
    run.module = kDummyModule;
    run.game_id = "dummy";
    run.data_dir = root / "games" / "dummy";
    run.save_dir = root / "saves" / "dummy";  // does not exist yet: run_engine creates it

    std::string error;
    CHECK(run_engine(run, error) == kExitOk);
    CHECK(error.empty());
    const std::string written = read_file(run.save_dir / "dummy-run.txt");
    CHECK(written.find("game=dummy") != std::string::npos);
    CHECK(written.find("argc=0") != std::string::npos);

    run.extra_args = {"--exit", "7"};
    CHECK(run_engine(run, error) == 7);
    CHECK(read_file(run.save_dir / "dummy-run.txt").find("argc=2") != std::string::npos);
}

void test_run_engine_errors()
{
    const fs::path root = temp_dir("run-errors");
    EngineRun run;
    run.module = root / "missing-engine.so";
    run.game_id = "dummy";
    run.save_dir = root / "saves";

    std::string error;
    CHECK(run_engine(run, error) == kExitModuleMissing);
    CHECK(!error.empty());

    run.module = kDummyModule;
    run.game_id = "sonic1";  // not a game the dummy module runs
    error.clear();
    CHECK(run_engine(run, error) == kExitModuleInvalid);
    CHECK(error.find("cannot run game") != std::string::npos);

    CHECK(describe_exit_code(kExitModuleMissing).find("could not be loaded") != std::string::npos);
    CHECK(describe_exit_code(42).find("42") != std::string::npos);
}

void test_child_process()
{
    const fs::path exe = self_executable();
    CHECK(!exe.empty() && fs::exists(exe));

    std::string error;
    CHECK(run_child(exe, {"__child", "0"}, error) == 0);
    CHECK(run_child(exe, {"__child", "9"}, error) == 9);
    // Arguments with spaces and quotes arrive intact.
    CHECK(run_child(exe, {"__child_echo", "a b \"c\"", "a b \"c\""}, error) == 0);

    CHECK(run_child(exe.parent_path() / "no-such-program", {}, error) != 0);
}


void test_ini()
{
    std::string text = "[Dev]\nDevMenu=false\nDataFile = Data.rsdk\n\n[Game]\nLanguage=0\n";
    text = ini_set(text, "Dev", "datafile", "../../games/sonic1/Data.rsdk");
    CHECK(ini_get(text, "Dev", "DataFile") == "../../games/sonic1/Data.rsdk");
    CHECK(ini_get(text, "Dev", "DevMenu") == "false");
    CHECK(text.find("Data.rsdk\n\n[Game]") != std::string::npos);  // replaced in place

    text = ini_set(text, "Game", "SkipStartMenu", "true");
    CHECK(ini_get(text, "Game", "SkipStartMenu") == "true");
    CHECK(ini_get(text, "Game", "Language") == "0");

    text = ini_set(text, "Window", "VSync", "true");
    CHECK(ini_get(text, "Window", "VSync") == "true");
    CHECK(ini_get(text, "Dev", "VSync").empty());

    CHECK(ini_get(ini_set("", "Dev", "DataFile", "x"), "Dev", "DataFile") == "x");
    CHECK(ini_get("[Dev]\r\nKey=1\r\n", "dev", "key") == "1");

    const fs::path dir = temp_dir("ini");
    CHECK(ini_set_file(dir / "settings.ini", "Dev", "DataFile", "a"));
    CHECK(ini_set_file(dir / "settings.ini", "Game", "SkipStartMenu", "false"));
    const std::string written = read_file(dir / "settings.ini");
    CHECK(ini_get(written, "Dev", "DataFile") == "a");
    CHECK(ini_get(written, "Game", "SkipStartMenu") == "false");
}

void test_saves_rsdkv4()
{
    const fs::path dir = temp_dir("saves-rsdkv4");
    CHECK(!rsdkv4::read_slots(dir));  // no save file yet

    std::vector<std::int32_t> ram(rsdkv4::kSaveRamInts, 0);
    // Slot 0: Knuckles, zone 3 (1-based) -> regular list, zone index 2.
    ram[0 * 8 + rsdkv4::kCharacter] = 2;
    ram[0 * 8 + rsdkv4::kLives] = 5;
    ram[0 * 8 + rsdkv4::kScore] = 12340;
    ram[0 * 8 + rsdkv4::kZone] = 3;
    ram[0 * 8 + rsdkv4::kEmeralds] = 4;
    // Slot 2: special stage 2 (129 + 1).
    ram[2 * 8 + rsdkv4::kCharacter] = 3;
    ram[2 * 8 + rsdkv4::kZone] = 130;
    // Something outside the slots, which delete must keep.
    ram[0x100] = 77;
    write_save_ram(dir / "SData.bin", ram);

    auto slots = rsdkv4::read_slots(dir);
    CHECK(slots.has_value());
    if (!slots)
        return;
    const rsdkv4::Slot &s0 = (*slots)[0];
    CHECK(!s0.empty && s0.character == 2 && s0.lives == 5 && s0.score == 12340 && s0.emeralds == 4);
    CHECK(s0.list == rsdkv4::kListRegular && s0.zone == 2);
    CHECK((*slots)[1].empty && (*slots)[1].zone == -1);
    CHECK(!(*slots)[2].empty && (*slots)[2].list == rsdkv4::kListSpecial && (*slots)[2].zone == 1);
    CHECK((*slots)[3].empty);

    std::string error;
    CHECK(rsdkv4::delete_slot(dir, 0, error));
    slots = rsdkv4::read_slots(dir);
    CHECK(slots && (*slots)[0].empty && !(*slots)[2].empty);
    CHECK(fs::file_size(dir / "SData.bin") == rsdkv4::kSaveRamInts * 4);
    std::ifstream in(dir / "SData.bin", std::ios::binary);
    in.seekg(0x100 * 4);
    unsigned char b[4] = {};
    in.read(reinterpret_cast<char *>(b), 4);
    CHECK(b[0] == 77 && b[1] == 0);
    CHECK(!rsdkv4::delete_slot(dir, 4, error));

    // The older SGame.bin is used when there is no SData.bin.
    const fs::path old = temp_dir("saves-rsdkv4-sgame");
    ram.assign(rsdkv4::kSaveRamInts, 0);
    ram[1 * 8 + rsdkv4::kZone] = 1;
    write_save_ram(old / "SGame.bin", ram);
    slots = rsdkv4::read_slots(old);
    CHECK(slots && !(*slots)[1].empty && (*slots)[1].zone == 0);
}

#ifdef RSDKAIR_RSDKV4_MODULE
void test_rsdkv4_module()
{
    std::string error;
    auto module = Module::open(kRsdkv4Module, error);
    CHECK(module != nullptr);
    if (!module) {
        std::fprintf(stderr, "%s\n", error.c_str());
        return;
    }
    auto games = module->function<RsdkAirEngineGamesFn>("rsdkair_engine_games");
    CHECK(games != nullptr);
    std::vector<std::string> ids;
    for (const RsdkAirGameInfo *const *g = games(); g && *g; ++g)
        ids.emplace_back((*g)->id);
    CHECK((ids == std::vector<std::string>{"sonic1", "sonic2"}));
    // Only the ABI is exported: tinyxml2, stb and the engine stay inside.
    CHECK(module->symbol("_ZN8tinyxml211XMLDocument5ClearEv") == nullptr);
    CHECK(module->symbol("Engine") == nullptr);

    // Without game data the engine fails cleanly, before opening any window, and the
    // settings it needs are in the save folder.
    const fs::path root = temp_dir("rsdkv4-module");
    EngineRun run;
    run.module = kRsdkv4Module;
    run.game_id = "sonic2";
    run.data_dir = root / "games" / "sonic2";
    run.save_dir = root / "saves" / "sonic2";
    fs::create_directories(run.data_dir);

    run.extra_args = {"--slot", "9"};
    CHECK(run_engine(run, error) == RSDKAIR_EXIT_BAD_ARGS);

    run.extra_args = {"--slot", "1", "--player", "2"};
    CHECK(run_engine(run, error) == RSDKAIR_EXIT_ENGINE_FAILED);
    const std::string settings = read_file(run.save_dir / "settings.ini");
    CHECK(ini_get(settings, "Dev", "DataFile") == "../../games/sonic2/Data.rsdk");
    CHECK(ini_get(settings, "Game", "SkipStartMenu") == "true");
}
#endif

} // namespace

int main(int argc, char **argv)
{
    // Helper modes used by test_child_process.
    if (argc == 3 && std::strcmp(argv[1], "__child") == 0)
        return std::atoi(argv[2]);
    if (argc == 4 && std::strcmp(argv[1], "__child_echo") == 0)
        return std::strcmp(argv[2], argv[3]) == 0 && std::strcmp(argv[2], "a b \"c\"") == 0 ? 0 : 1;

    const std::map<std::string, std::function<void()>> tests = {
        {"catalog", test_catalog},
        {"cli", test_cli},
        {"rom", test_rom},
        {"detect", test_detect},
        {"windows_quoting", test_windows_quoting},
        {"module_hidden_symbols", test_module_hidden_symbols},
        {"run_engine", test_run_engine},
        {"run_engine_errors", test_run_engine_errors},
        {"child_process", test_child_process},
        {"ini", test_ini},
        {"saves_rsdkv4", test_saves_rsdkv4},
#ifdef RSDKAIR_RSDKV4_MODULE
        {"rsdkv4_module", test_rsdkv4_module},
#endif
    };

    std::vector<std::string> selected;
    for (int i = 1; i < argc; ++i)
        selected.emplace_back(argv[i]);
    if (selected.empty()) {
        for (const auto &[name, fn] : tests)
            selected.push_back(name);
    }

    for (const std::string &name : selected) {
        auto it = tests.find(name);
        if (it == tests.end()) {
            std::fprintf(stderr, "unknown test: %s\n", name.c_str());
            return 2;
        }
        const int before = g_failures;
        it->second();
        std::printf("%s %s\n", g_failures == before ? "PASS" : "FAIL", name.c_str());
    }
    return g_failures == 0 ? 0 : 1;
}
