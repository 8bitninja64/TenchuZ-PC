#pragma once
#include <cstdint>
#include <filesystem>
#include <vector>

namespace tz {

// Decoded retail XEX2: the raw header and the loaded PE image.
struct XexImage {
    std::vector<uint8_t> header;
    std::vector<uint8_t> image;
    uint32_t loadAddress = 0;
    uint32_t entryPoint = 0;
    // Optional header field: a pointer into `header` for keyed blobs, or the
    // inline value for keys whose low byte is 0 or 1. Returns false if absent.
    bool field(uint32_t key, uint32_t& value) const;
};

XexImage loadXex(const std::filesystem::path& path);

}  // namespace tz
