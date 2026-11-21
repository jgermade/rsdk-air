#include <rsdkair/rom.hpp>

#include <algorithm>
#include <fstream>
#include <string_view>
#include <system_error>
#include <vector>

namespace rsdkair {

namespace {

constexpr std::string_view kRomFile = "Sonic_Knuckles_wSonic3.bin";

std::string trim(std::string text)
{
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

} // namespace

Check check_s3k_rom(std::uintmax_t size, std::span<const std::uint8_t> header)
{
    if (size == 0)
        return {false, "no ROM: Sonic 3 A.I.R. needs " + std::string(kRomFile) + " (Sonic & Knuckles + Sonic 3)"};

    std::string serial;
    if (header.size() > kS3kSerialOffset) {
        const auto count = std::min(kS3kSerialLength, header.size() - kS3kSerialOffset);
        for (std::size_t i = 0; i < count; ++i)
            serial.push_back(static_cast<char>(header[kS3kSerialOffset + i]));
    }
    const bool lock_on = serial.find("MK-1563") != std::string::npos;

    if (size == kS3kRomSize && lock_on)
        return {true, {}};
    if (size == kS3kRomSize / 2)
        return {false, "this is Sonic 3 or Sonic & Knuckles on its own (2 MB); Sonic 3 A.I.R. needs both locked on: "
                       "\"Sonic & Knuckles + Sonic 3\" (" +
                           std::string(kRomFile) + ", 4 MB)"};
    if (size != kS3kRomSize)
        return {false, "the ROM is " + std::to_string(size) + " bytes; " + std::string(kRomFile) + " has " +
                           std::to_string(kS3kRomSize) + " (4 MB)"};
    return {false, "not Sonic & Knuckles + Sonic 3 (serial \"" + trim(serial) + "\", expected MK-1563)"};
}

Check check_s3k_rom_file(const std::filesystem::path &path)
{
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec)
        return check_s3k_rom(0, {});

    std::ifstream in(path, std::ios::binary);
    std::vector<std::uint8_t> header(kS3kSerialOffset + kS3kSerialLength);
    in.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
    header.resize(static_cast<std::size_t>(in.gcount()));
    return check_s3k_rom(size, header);
}

} // namespace rsdkair
