// Minimal dependency-free image loading: BMP (24/32-bit uncompressed) and binary PPM (P6).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrr {

struct ImageData {
    int width = 0, height = 0;
    std::vector<uint32_t> rgba;  // R in the low byte
    bool valid() const { return width > 0 && height > 0; }
};

bool loadImage(const std::string& path, ImageData& out, std::string& err);

}  // namespace vrr
