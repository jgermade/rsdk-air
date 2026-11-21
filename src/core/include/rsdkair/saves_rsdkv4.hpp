// RSDKv4 save slots (Sonic 1 / Sonic 2), read straight from the save file so the
// launcher can show them without loading the engine.
//
// SData.bin (or the older SGame.bin) is the engine's saveRAM: 0x2000 little-endian
// int32. Slot n uses ints [n*8, n*8+8) with the layout of Debug.cpp's STARTMENU_SAVESEL
// (the same one rsdkv4-wasm's bridge reads).
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rsdkair::rsdkv4 {

inline constexpr int kSlotCount = 4;
inline constexpr std::size_t kSaveRamInts = 0x2000;

enum SaveField : int {
    kCharacter = 0,  // 0 Sonic, 1 Tails, 2 Knuckles, 3 Sonic & Tails
    kLives,
    kScore,
    kBonus,
    kZone,           // 1-based next zone; 0 = unused slot; >127 = special stage
    kEmeralds,
    kSpecialPos,
};

enum StageList : int { kListNone = -1, kListRegular = 1, kListSpecial = 3 };

struct Slot {
    int slot = 0;
    bool empty = true;
    int character = 0;
    int lives = 0;
    int score = 0;
    int emeralds = 0;   // count or bit mask, as the game stored it
    int list = kListNone;
    int zone = -1;      // 0-based index into `list`, -1 when empty
};

// Decodes slot `slot` (0-3) from saveRAM.
Slot decode_slot(std::span<const std::int32_t> ram, int slot);

// The save file the engine uses in save_dir: SData.bin, else SGame.bin. Empty if neither.
std::filesystem::path save_file(const std::filesystem::path &save_dir);

// The four slots, or nullopt if there is no save file yet (all slots empty).
std::optional<std::array<Slot, kSlotCount>> read_slots(const std::filesystem::path &save_dir);

// Clears a slot in the save file exactly as the engine's own delete does. False with
// error set if the file cannot be read or written.
bool delete_slot(const std::filesystem::path &save_dir, int slot, std::string &error);

// Where a saved slot continues, for the slot screen. Same table as rsdkv4-wasm's app
// (STAGES, from craft's game.service.js): each zone's first scene in the regular stage
// list; a save points at an act (Scrap Brain 3 is scene 17).
struct SlotZone {
    std::string name;  // "AQUATIC RUIN", "SPECIAL STAGE", "UNKNOWN ZONE"
    std::string tag;   // "ZONE 3", "STAGE 2", or empty
    int act = 0;       // 1-based act within the zone, 0 when not applicable
    std::string art;   // portrait under art/, e.g. "stages/sonic2/zone-3.png"; may be empty
};

// Nothing for an empty slot. game_id is "sonic1" or "sonic2".
std::optional<SlotZone> describe_slot(std::string_view game_id, const Slot &slot);

// How many Chaos Emeralds a slot has. RSDKv4 stores how many, not which; a value out of
// range is counted by bits (as rsdkv4-wasm does). At most emerald_total(game_id).
int emerald_count(std::string_view game_id, const Slot &slot);
int emerald_total(std::string_view game_id);  // 6 in Sonic 1, 7 in Sonic 2

// "Sonic", "Tails", "Knuckles", "Sonic & Tails".
std::string_view character_name(int character);

} // namespace rsdkair::rsdkv4
