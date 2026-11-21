#include <rsdkair/catalog.hpp>

#include <array>

namespace rsdkair {

namespace {

constexpr std::array<std::string_view, 1> kDataRsdk = {"Data.rsdk"};
constexpr std::array<std::string_view, 1> kS3kRom = {"Sonic_Knuckles_wSonic3.bin"};

constexpr std::array<GameSpec, 5> kGames = {{
    {"sonic1", "Sonic the Hedgehog", "rsdkv4", kDataRsdk, Validator::None, 1991},
    {"sonic2", "Sonic the Hedgehog 2", "rsdkv4", kDataRsdk, Validator::None, 1992},
    {"soniccd", "Sonic CD", "rsdkv3", kDataRsdk, Validator::None, 1993},
    {"s3air", "Sonic 3 A.I.R.", "sonic3air", kS3kRom, Validator::S3kLockOn, 1994},
    {"mania", "Sonic Mania", "rsdkv5", kDataRsdk, Validator::None, 2017},
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
