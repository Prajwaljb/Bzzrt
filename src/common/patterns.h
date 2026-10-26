// CPU reference implementation of the test patterns (used by the DRM/KMS
// backend, which draws with the CPU). The GPU shaders implement the same math.
#pragma once

#include <cstdint>

#include "image_load.h"

namespace vrr {

constexpr int kMoveBarWidth = 64;
constexpr double kMoveSpeedPxPerSec = 960.0;
constexpr int kCounterBits = 12;
constexpr int kCounterCell = 24;
constexpr int kCounterGap = 4;

// Left edge of the white bar; the black bar is half a period further.
double moveBarX(double timeSec, int width);
void moveBars(double moveX, int width, double& whiteX, double& blackX);

// Rectangle (x, y, w, h) of the frame-counter strip.
void counterRect(int width, int& x, int& y, int& w, int& h);

struct Rgb {
    float r, g, b;
};

// Static (time-independent) part of a pattern; moving bars and the counter are drawn separately.
Rgb staticPatternColor(int pattern, int x, int y, int width, int height, const ImageData* image);

}  // namespace vrr
