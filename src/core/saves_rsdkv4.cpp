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

} // namespace rsdkair::rsdkv4
