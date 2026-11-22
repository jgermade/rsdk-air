#include <rsdkair/saves_rsdkv4.hpp>

#include <rsdkair/paths.hpp>

#include <algorithm>
#include <fstream>
#include <system_error>
#include <vector>

namespace rsdkair::rsdkv4 {

namespace fs = std::filesystem;

namespace {

std::vector<std::int32_t> read_ram(const fs::path &file)
{
    std::vector<std::int32_t> ram(kSaveRamInts, 0);
    std::ifstream in(file, std::ios::binary);
    std::vector<unsigned char> bytes(kSaveRamInts * 4, 0);
    in.read(reinterpret_cast<char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    for (std::size_t i = 0; i < kSaveRamInts; ++i) {
        const std::uint32_t v = static_cast<std::uint32_t>(bytes[i * 4]) | (static_cast<std::uint32_t>(bytes[i * 4 + 1]) << 8) |
                                (static_cast<std::uint32_t>(bytes[i * 4 + 2]) << 16) |
                                (static_cast<std::uint32_t>(bytes[i * 4 + 3]) << 24);
        ram[i] = static_cast<std::int32_t>(v);
    }
    return ram;
}

} // namespace

Slot decode_slot(std::span<const std::int32_t> ram, int slot)
{
    Slot result;
    result.slot = slot;
    const std::size_t base = static_cast<std::size_t>(slot) * 8;
    if (slot < 0 || slot >= kSlotCount || ram.size() < base + 8)
        return result;

    const int zone = ram[base + kZone];
    result.character = ram[base + kCharacter];
    result.lives = ram[base + kLives];
    result.score = ram[base + kScore];
    result.emeralds = ram[base + kEmeralds];
    if (zone != 0) {
        const bool special = zone > 127;
        result.empty = false;
        result.list = special ? kListSpecial : kListRegular;
        result.zone = special ? zone - 129 : zone - 1;
    }
    return result;
}

fs::path save_file(const fs::path &save_dir)
{
    std::error_code ec;
    for (const char *name : {"SData.bin", "SGame.bin"}) {
        const fs::path path = save_dir / name;
        if (fs::is_regular_file(path, ec))
            return path;
    }
    return {};
}

std::optional<std::array<Slot, kSlotCount>> read_slots(const fs::path &save_dir)
{
    const fs::path file = save_file(save_dir);
    if (file.empty())
        return std::nullopt;
    const std::vector<std::int32_t> ram = read_ram(file);
    std::array<Slot, kSlotCount> slots;
    for (int i = 0; i < kSlotCount; ++i)
        slots[static_cast<std::size_t>(i)] = decode_slot(ram, i);
    return slots;
}

bool delete_slot(const fs::path &save_dir, int slot, std::string &error)
{
    if (slot < 0 || slot >= kSlotCount) {
        error = "invalid slot " + std::to_string(slot);
        return false;
    }
    const fs::path file = save_file(save_dir);
    if (file.empty())
        return true;  // nothing saved, nothing to delete

    std::vector<std::int32_t> ram = read_ram(file);
    const std::size_t base = static_cast<std::size_t>(slot) * 8;
    // Same values as the engine's own delete (rsdkv4-wasm's web_delete_save).
    ram[base + kCharacter] = 0;
    ram[base + kLives] = 3;
    ram[base + kScore] = 0;
    ram[base + kBonus] = 50000;
    ram[base + kZone] = 0;
    ram[base + kEmeralds] = 0;
    ram[base + kSpecialPos] = 0;
    ram[base + 7] = 0;

    std::vector<unsigned char> bytes(kSaveRamInts * 4);
    for (std::size_t i = 0; i < kSaveRamInts; ++i) {
        const auto v = static_cast<std::uint32_t>(ram[i]);
        bytes[i * 4] = static_cast<unsigned char>(v);
        bytes[i * 4 + 1] = static_cast<unsigned char>(v >> 8);
        bytes[i * 4 + 2] = static_cast<unsigned char>(v >> 16);
        bytes[i * 4 + 3] = static_cast<unsigned char>(v >> 24);
    }
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) {
        error = "cannot write " + path_to_utf8(file);
        return false;
    }
    return true;
}

namespace {

// rsdkv4-wasm's STAGES table: zone name and first scene, in the regular stage list.
struct Zone {
    std::string_view name;
    int first_scene;
};

struct Pack {
    std::string_view folder;  // under art/stages/
    int scenes;               // regular-list scenes the table covers
    int art;                  // zones 1..art have a portrait
    std::span<const Zone> zones;
};

constexpr Zone kSonic1Zones[] = {
    {"GREEN HILL", 0}, {"MARBLE", 3},       {"SPRING YARD", 6}, {"LABYRINTH", 9},
    {"STARLIGHT", 12}, {"SCRAP BRAIN", 15}, {"FINAL", 18},
};

constexpr Zone kSonic2Zones[] = {
    {"EMERALD HILL", 0},   {"CHEMICAL PLANT", 2}, {"AQUATIC RUIN", 4}, {"CASINO NIGHT", 6},
    {"HILL TOP", 8},       {"MYSTIC CAVE", 10},   {"OIL OCEAN", 12},   {"METROPOLIS", 14},
    {"SKY CHASE", 17},     {"WING FORTRESS", 18}, {"DEATH EGG", 19},   {"HIDDEN PALACE", 20},
};

constexpr Pack kSonic1 = {"sonic1", 19, 7, kSonic1Zones};
constexpr Pack kSonic2 = {"sonic2", 21, 10, kSonic2Zones};

const Pack &pack_for(std::string_view game_id) { return game_id == "sonic2" ? kSonic2 : kSonic1; }

} // namespace

std::optional<SlotZone> describe_slot(std::string_view game_id, const Slot &slot)
{
    if (slot.empty)
        return std::nullopt;
    const Pack &pack = pack_for(game_id);
    const std::string folder = "stages/" + std::string(pack.folder) + "/";
    SlotZone zone;
    if (slot.list == kListSpecial) {
        zone.name = "SPECIAL STAGE";
        zone.tag = "STAGE " + std::to_string(slot.zone + 1);
        zone.art = folder + "special.png";
        return zone;
    }
    if (slot.zone < 0 || slot.zone >= pack.scenes) {
        zone.name = "UNKNOWN ZONE";
        return zone;
    }
    std::size_t index = 0;
    for (std::size_t i = 0; i < pack.zones.size(); ++i) {
        if (slot.zone >= pack.zones[i].first_scene)
            index = i;
    }
    zone.name = std::string(pack.zones[index].name);
    zone.tag = "ZONE " + std::to_string(index + 1);
    zone.act = slot.zone - pack.zones[index].first_scene + 1;
    if (static_cast<int>(index) < pack.art)
        zone.art = folder + "zone-" + std::to_string(index + 1) + ".png";
    return zone;
}

int emerald_total(std::string_view game_id) { return game_id == "sonic1" ? 6 : 7; }

int emerald_count(std::string_view game_id, const Slot &slot)
{
    if (slot.empty)
        return 0;
    int count = slot.emeralds;
    if (count < 0 || count > 7) {
        count = 0;
        for (unsigned m = static_cast<unsigned>(slot.emeralds); m; m &= m - 1)
            ++count;
    }
    return std::min(count, emerald_total(game_id));
}

std::string_view character_name(int character)
{
    switch (character) {
    case 1: return "Tails";
    case 2: return "Knuckles";
    case 3: return "Sonic & Tails";
    default: return "Sonic";
    }
}

} // namespace rsdkair::rsdkv4
