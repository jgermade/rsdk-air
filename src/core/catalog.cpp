#include <rsdkair/catalog.hpp>

#include <array>

namespace rsdkair {

namespace {

constexpr std::array<std::string_view, 1> kDataRsdk = {"Data.rsdk"};
constexpr std::array<std::string_view, 1> kS3kRom = {"Sonic_Knuckles_wSonic3.bin"};

constexpr std::array<GameSpec, 5> kGames = {{
    {"sonic1", "Sonic the Hedgehog", "rsdkv4", kDataRsdk, Validator::None},
    {"sonic2", "Sonic the Hedgehog 2", "rsdkv4", kDataRsdk, Validator::None},
    {"soniccd", "Sonic CD", "rsdkv3", kDataRsdk, Validator::None},
    {"s3air", "Sonic 3 A.I.R.", "sonic3air", kS3kRom, Validator::S3kLockOn},
    {"mania", "Sonic Mania", "rsdkv5", kDataRsdk, Validator::None},
}};

} // namespace

std::span<const GameSpec> catalog() { return kGames; }

const GameSpec *find_game(std::string_view id)
{
    for (const GameSpec &game : kGames) {
        if (game.id == id)
            return &game;
    }
    return nullptr;
}

} // namespace rsdkair
