#include "overlay.h"

#include <algorithm>

#include "font8x8.h"

namespace vrr {

Overlay::Overlay() : px_(static_cast<size_t>(kOverlayW) * kOverlayH, 0u) {}

void Overlay::clear() { std::fill(px_.begin(), px_.end(), 0u); }

void Overlay::fillRect(int x, int y, int w, int h, uint32_t color) {
    const int x0 = std::max(0, x), y0 = std::max(0, y);
    const int x1 = std::min(kOverlayW, x + w), y1 = std::min(kOverlayH, y + h);
    for (int yy = y0; yy < y1; ++yy) {
        uint32_t* row = px_.data() + static_cast<size_t>(yy) * kOverlayW;
        std::fill(row + x0, row + std::max(x0, x1), color);
    }
}

int Overlay::text(int x, int y, const std::string& s, uint32_t color, int scale) {
    for (unsigned char ch : s) {
        if (ch < 32 || ch > 127) ch = '?';
        const uint8_t* glyph = kFont8x8[ch - 32];
        for (int gy = 0; gy < 8; ++gy) {
            for (int gx = 0; gx < 8; ++gx) {
                if (glyph[gy] & (1u << gx)) fillRect(x + gx * scale, y + gy * scale, scale, scale, color);
            }
        }
        x += 8 * scale;
    }
    return x;
}

}  // namespace vrr
