#include "patterns.h"

#include <algorithm>
#include <cmath>

#include "backend.h"

namespace vrr {

const char* patternName(int p) {
    switch (p) {
        case kPatMidGray: return "Mid gray (50%)";
        case kPatDarkGray: return "Dark gray (20%)";
        case kPatWhite: return "White";
        case kPatBlack: return "Black";
        case kPatGradient: return "Bright gradient";
        case kPatHighContrast: return "Static high-contrast";
        case kPatMoving: return "Moving bars";
        case kPatImage: return "User image";
        default: return "?";
    }
}

const char* windowModeName(WindowMode m) {
    switch (m) {
        case WindowMode::Windowed: return "Windowed";
        case WindowMode::Borderless: return "Borderless fullscreen";
        case WindowMode::Exclusive: return "Exclusive fullscreen";
    }
    return "?";
}

double moveBarX(double t, int width) {
    const double period = static_cast<double>(width + kMoveBarWidth);
    return std::fmod(t * kMoveSpeedPxPerSec, period) - kMoveBarWidth;
}

void moveBars(double moveX, int width, double& whiteX, double& blackX) {
    const double period = static_cast<double>(width + kMoveBarWidth);
    whiteX = moveX;
    blackX = moveX + period * 0.5;
    if (blackX >= width) blackX -= period;
}

void counterRect(int width, int& x, int& y, int& w, int& h) {
    w = kCounterBits * (kCounterCell + kCounterGap) + kCounterGap;
    h = kCounterCell + 2 * kCounterGap;
    x = width - w - 16;
    y = 16;
}

Rgb staticPatternColor(int pattern, int x, int y, int width, int height, const ImageData* image) {
    switch (pattern) {
        case kPatMidGray: return {0.5f, 0.5f, 0.5f};
        case kPatDarkGray: return {0.2f, 0.2f, 0.2f};
        case kPatWhite: return {1.0f, 1.0f, 1.0f};
        case kPatBlack: return {0.0f, 0.0f, 0.0f};
        case kPatGradient: {
            const float v = 0.35f + 0.65f * (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
            return {v, v, v};
        }
        case kPatHighContrast: {
            const bool c = (((x / 64) + (y / 64)) & 1) != 0;
            const float v = c ? 1.0f : 0.0f;
            return {v, v, v};
        }
        case kPatMoving: return {0.5f, 0.5f, 0.5f};
        case kPatImage: {
            if (!image || !image->valid()) return {0.2f, 0.0f, 0.2f};
            const float s = std::min(static_cast<float>(width) / image->width,
                                     static_cast<float>(height) / image->height);
            const float dw = image->width * s, dh = image->height * s;
            const float ox = (width - dw) * 0.5f, oy = (height - dh) * 0.5f;
            const float u = (x + 0.5f - ox) / dw, v = (y + 0.5f - oy) / dh;
            if (u < 0 || v < 0 || u >= 1 || v >= 1) return {0, 0, 0};
            const int ix = std::min(image->width - 1, static_cast<int>(u * image->width));
            const int iy = std::min(image->height - 1, static_cast<int>(v * image->height));
            const uint32_t p = image->rgba[static_cast<size_t>(iy) * image->width + ix];
            return {(p & 255) / 255.0f, ((p >> 8) & 255) / 255.0f, ((p >> 16) & 255) / 255.0f};
        }
        default: return {0.5f, 0.5f, 0.5f};
    }
}

}  // namespace vrr
