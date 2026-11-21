#include <rsdkair/saves_rsdkv4.hpp>

#include <rsdkair/paths.hpp>

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

// Zones by position in the regular stage list, as in the Retro Engine remasters'
// GameConfig (acts listed one after another): `first` is the zone's first act. Not
// checked against every data version; unknown positions fall back to "Stage N".
struct ZoneRange {
    int first;
    int last;
    std::string_view name;
};

constexpr ZoneRange kSonic1Zones[] = {
    {0, 2, "Green Hill Zone"}, {3, 5, "Marble Zone"},     {6, 8, "Spring Yard Zone"}, {9, 11, "Labyrinth Zone"},
    {12, 14, "Star Light Zone"}, {15, 17, "Scrap Brain Zone"}, {18, 18, "Final Zone"},
};

constexpr ZoneRange kSonic2Zones[] = {
    {0, 1, "Emerald Hill Zone"},   {2, 3, "Chemical Plant Zone"}, {4, 5, "Aquatic Ruin Zone"},
    {6, 7, "Casino Night Zone"},   {8, 9, "Hill Top Zone"},       {10, 11, "Mystic Cave Zone"},
    {12, 13, "Oil Ocean Zone"},    {14, 16, "Metropolis Zone"},   {17, 17, "Sky Chase Zone"},
    {18, 18, "Wing Fortress Zone"}, {19, 19, "Death Egg Zone"},
};

} // namespace

std::string zone_label(std::string_view game_id, const Slot &slot)
{
    if (slot.empty)
        return {};
    if (slot.list == kListSpecial)
        return "Special Stage " + std::to_string(slot.zone + 1);
    std::span<const ZoneRange> zones;
    if (game_id == "sonic1")
        zones = kSonic1Zones;
    else if (game_id == "sonic2")
        zones = kSonic2Zones;
    for (const ZoneRange &z : zones) {
        if (slot.zone >= z.first && slot.zone <= z.last)
            return std::string(z.name);
    }
    return "Stage " + std::to_string(slot.zone + 1);
}

int emerald_total(std::string_view game_id) { return game_id == "sonic1" ? 6 : 7; }

int emerald_count(std::string_view game_id, const Slot &slot)
{
    if (slot.empty)
        return 0;
    const unsigned mask = static_cast<unsigned>(slot.emeralds) & ((1u << emerald_total(game_id)) - 1);
    int count = 0;
    for (unsigned m = mask; m; m &= m - 1)
        ++count;
    return count;
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
