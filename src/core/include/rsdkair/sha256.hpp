// SHA-256, to check downloads against the hashes pinned in the code.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

namespace rsdkair {

class Sha256 {
public:
    Sha256();
    void update(const void *data, std::size_t size);
    std::string hex();  // finishes the hash; lowercase hex, 64 characters

private:
    void block(const std::uint8_t *p);
    std::uint32_t h_[8];
    std::uint8_t buf_[64];
    std::size_t used_ = 0;
    std::uint64_t total_ = 0;
};

// Lowercase hex SHA-256 of a file, or "" if it cannot be read.
std::string sha256_file(const std::filesystem::path &path);

} // namespace rsdkair
