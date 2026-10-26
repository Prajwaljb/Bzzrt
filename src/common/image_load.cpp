#include "image_load.h"

#include <cctype>
#include <cstdio>
#include <cstring>

namespace vrr {

static bool readFile(const std::string& path, std::vector<uint8_t>& data) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        std::fclose(f);
        return false;
    }
    data.resize(static_cast<size_t>(size));
    const size_t got = std::fread(data.data(), 1, data.size(), f);
    std::fclose(f);
    return got == data.size();
}

static uint32_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static uint32_t rd32(const uint8_t* p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

static bool loadBmp(const std::vector<uint8_t>& d, ImageData& out, std::string& err) {
    if (d.size() < 54) return err = "BMP too small", false;
    const uint32_t dataOff = rd32(&d[10]);
    const int32_t w = static_cast<int32_t>(rd32(&d[18]));
    const int32_t hRaw = static_cast<int32_t>(rd32(&d[22]));
    const uint32_t bpp = rd16(&d[28]);
    const uint32_t comp = rd32(&d[30]);
    if (w <= 0 || hRaw == 0 || w > 16384 || hRaw > 16384 || hRaw < -16384)
        return err = "BMP has unsupported dimensions", false;
    if (!((bpp == 24 && comp == 0) || (bpp == 32 && (comp == 0 || comp == 3))))
        return err = "only uncompressed 24/32-bit BMP is supported", false;
    const bool bottomUp = hRaw > 0;
    const int h = bottomUp ? hRaw : -hRaw;
    const size_t stride = ((static_cast<size_t>(bpp) * w + 31) / 32) * 4;
    if (dataOff + stride * h > d.size()) return err = "BMP truncated", false;
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w) * h);
    const size_t bytes = bpp / 8;
    for (int y = 0; y < h; ++y) {
        const uint8_t* row = &d[dataOff + stride * (bottomUp ? (h - 1 - y) : y)];
        for (int x = 0; x < w; ++x) {
            const uint8_t* p = row + x * bytes;  // B, G, R
            out.rgba[static_cast<size_t>(y) * w + x] = p[2] | (p[1] << 8) | (p[0] << 16) | 0xFF000000u;
        }
    }
    return true;
}

static bool loadPpm(const std::vector<uint8_t>& d, ImageData& out, std::string& err) {
    size_t pos = 2;
    auto token = [&](int& v) -> bool {
        for (;;) {
            while (pos < d.size() && std::isspace(d[pos])) ++pos;
            if (pos < d.size() && d[pos] == '#') {
                while (pos < d.size() && d[pos] != '\n') ++pos;
                continue;
            }
            break;
        }
        if (pos >= d.size() || !std::isdigit(d[pos])) return false;
        v = 0;
        while (pos < d.size() && std::isdigit(d[pos])) v = v * 10 + (d[pos++] - '0');
        return true;
    };
    int w = 0, h = 0, maxv = 0;
    if (!token(w) || !token(h) || !token(maxv)) return err = "bad PPM header", false;
    ++pos;  // single whitespace after maxval
    if (w <= 0 || h <= 0 || w > 16384 || h > 16384 || maxv <= 0 || maxv > 255)
        return err = "unsupported PPM (need 8-bit P6)", false;
    if (pos + static_cast<size_t>(w) * h * 3 > d.size()) return err = "PPM truncated", false;
    out.width = w;
    out.height = h;
    out.rgba.resize(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < out.rgba.size(); ++i) {
        const uint8_t* p = &d[pos + i * 3];
        out.rgba[i] = p[0] | (p[1] << 8) | (p[2] << 16) | 0xFF000000u;
    }
    return true;
}

bool loadImage(const std::string& path, ImageData& out, std::string& err) {
    std::vector<uint8_t> d;
    if (!readFile(path, d)) return err = "cannot read " + path, false;
    if (d.size() >= 2 && d[0] == 'B' && d[1] == 'M') return loadBmp(d, out, err);
    if (d.size() >= 2 && d[0] == 'P' && d[1] == '6') return loadPpm(d, out, err);
    err = "unsupported format (use 24/32-bit BMP or binary PPM; e.g. `magick in.png out.bmp`)";
    return false;
}

}  // namespace vrr
