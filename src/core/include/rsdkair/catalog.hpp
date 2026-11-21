// The games rsdk-air knows about. The launcher reads this table instead of loading
// engine modules, so no engine is ever loaded into the launcher's process.
#pragma once

#include <span>
#include <string_view>

namespace rsdkair {

enum class Validator {
    None,       // the required files only have to exist
    S3kLockOn,  // Sonic & Knuckles + Sonic 3 ROM (4 MB, serial MK-1563)
};

struct GameSpec {
    std::string_view id;      // also the folder name under games/ and saves/
    std::string_view title;
    std::string_view module;  // engine module, e.g. "rsdkv4" -> engines/rsdkv4.so
    std::span<const std::string_view> required;  // relative to games/<id>/
    Validator validator;
    int year;                 // original release, shown under the cover in the picker
};

// All known games, in the order the picker shows them.
std::span<const GameSpec> catalog();

// nullptr if the id is unknown.
const GameSpec *find_game(std::string_view id);

} // namespace rsdkair
