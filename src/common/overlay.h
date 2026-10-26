// CPU-rasterized statistics overlay (RGBA8), uploaded to the GPU at ~10 Hz.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrr {

constexpr int kOverlayW = 1280;
constexpr int kOverlayH = 960;

constexpr uint32_t rgba8(uint32_t r, uint32_t g, uint32_t b, uint32_t a = 255) {
    return r | (g << 8) | (b << 16) | (a << 24);
}

class Overlay {
public:
    Overlay();
    void clear();
    void fillRect(int x, int y, int w, int h, uint32_t color);
    // Draws text; returns x after the last glyph.
    int text(int x, int y, const std::string& s, uint32_t color, int scale);
    void commit(int usedW, int usedH) {
        usedW_ = usedW;
        usedH_ = usedH;
        ++version_;
    }

    const uint32_t* pixels() const { return px_.data(); }
    uint64_t version() const { return version_; }
    int usedW() const { return usedW_; }
    int usedH() const { return usedH_; }

private:
    std::vector<uint32_t> px_;
    uint64_t version_ = 0;
    int usedW_ = 0, usedH_ = 0;
};

}  // namespace vrr
