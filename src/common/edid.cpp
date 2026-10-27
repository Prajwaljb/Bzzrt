#include "edid.h"

#include <cstdio>

namespace vrr {

namespace {

std::string descriptorText(const uint8_t* d) {
    std::string s;
    for (int k = 5; k < 18 && d[k] != 0x0A && d[k] != 0; ++k)
        if (d[k] >= 0x20 && d[k] < 0x7F) s += static_cast<char>(d[k]);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

void parseBase(const uint8_t* b, EdidInfo& e) {
    const uint16_t m = static_cast<uint16_t>(b[8] << 8 | b[9]);
    for (int shift : {10, 5, 0}) {
        const int c = (m >> shift) & 0x1F;
        e.pnpId += (c >= 1 && c <= 26) ? static_cast<char>('A' + c - 1) : '?';
    }
    e.product = static_cast<uint16_t>(b[10] | b[11] << 8);
    e.serial = static_cast<uint32_t>(b[12] | b[13] << 8 | b[14] << 16 | static_cast<uint32_t>(b[15]) << 24);
    for (int off : {54, 72, 90, 108}) {
        const uint8_t* d = b + off;
        if (d[0] || d[1] || d[2]) continue;  // detailed timing, not a display descriptor
        if (d[3] == 0xFC) e.name = descriptorText(d);
        else if (d[3] == 0xFF) e.serialText = descriptorText(d);
        else if (d[3] == 0xFD) {
            // Byte 4 bits 1:0 add 255 to the vertical limits (EDID 1.4): 10 = max only, 11 = min and max.
            e.rangeMin = d[5] + ((d[4] & 3) == 3 ? 255 : 0);
            e.rangeMax = d[6] + ((d[4] & 2) ? 255 : 0);
        }
    }
}

void parseHdmiVrr(const uint8_t* x, int len, EdidInfo& e) {
    // HF-VSDB / HF-SCDB, offsets from the block header: byte 9 = VRRmax[9:8] | VRRmin, byte 10 = VRRmax[7:0].
    if (len < 10) return;
    const int vmin = x[9] & 0x3F;
    const int vmax = (x[9] & 0xC0) << 2 | x[10];
    if (vmin > 0) {
        e.hdmiVrrMin = vmin;
        e.hdmiVrrMax = vmax;  // 0: up to the highest refresh rate of the video formats
    }
}

void parseCta(const uint8_t* b, EdidInfo& e) {
    const int end = b[2];
    if (end < 4 || end > 127) return;
    for (int i = 4; i < end;) {
        const uint8_t* x = b + i;
        const int tag = x[0] >> 5, len = x[0] & 0x1F;
        if (i + 1 + len > end) break;
        if (tag == 3 && len >= 3) {
            const uint32_t oui = static_cast<uint32_t>(x[1] | x[2] << 8 | x[3] << 16);
            // AMD (00-00-1A): version at 4-5, then minimum and maximum refresh rate.
            if (oui == 0x00001A && len >= 7 && x[6] > 0 && x[7] > x[6]) {
                e.freesyncMin = x[6];
                e.freesyncMax = x[7];
            } else if (oui == 0xC45DD8) {
                parseHdmiVrr(x, len, e);
            }
        } else if (tag == 7 && len >= 1 && x[1] == 0x79) {
            parseHdmiVrr(x, len, e);  // HF-SCDB has the HF-VSDB layout
        }
        i += 1 + len;
    }
}

void parseDisplayId(const uint8_t* b, EdidInfo& e) {
    // DisplayID section inside an EDID extension: b[1] version, b[2] payload bytes, data blocks from b[5].
    if (b[1] < 0x20) return;  // Adaptive-Sync data block exists from DisplayID 2.0
    const int end = 5 + b[2];
    if (end > 127) return;
    for (int i = 5; i + 3 <= end;) {
        const uint8_t* x = b + i;
        const int len = x[2];
        if (len == 0 && x[0] == 0) break;  // padding
        if (i + 3 + len > end) break;
        if (x[0] == 0x2B) {
            const int size = 6 + ((x[1] >> 4) & 7);
            for (int d = 0; d + size <= len; d += size) {
                const uint8_t* q = x + 3 + d;
                const int vmin = q[2];
                const int vmax = 1 + (q[3] | (q[4] & 3) << 8);
                if (vmin > 0 && vmax > vmin && (e.adaptiveMin == 0 || vmax > e.adaptiveMax)) {
                    e.adaptiveMin = vmin;
                    e.adaptiveMax = vmax;
                }
            }
        }
        i += 3 + len;
    }
}

}  // namespace

EdidInfo parseEdid(const std::vector<uint8_t>& edid) {
    EdidInfo e;
    static const uint8_t header[8] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};
    if (edid.size() < 128) return e;
    for (int i = 0; i < 8; ++i)
        if (edid[i] != header[i]) return e;
    e.valid = true;
    parseBase(edid.data(), e);
    const size_t blocks = edid.size() / 128;
    for (size_t n = 1; n < blocks; ++n) {
        const uint8_t* b = edid.data() + n * 128;
        if (b[0] == 0x02) parseCta(b, e);
        else if (b[0] == 0x70) parseDisplayId(b, e);
    }
    return e;
}

bool edidVrrRange(const EdidInfo& e, double& minHz, double& maxHz, std::string& source) {
    auto use = [&](double lo, double hi, const char* src) {
        if (lo < 1 || hi < lo + 5) return false;  // a few Hz of tolerance is a fixed-refresh monitor, not VRR
        minHz = lo;
        maxHz = hi;
        source = src;
        return true;
    };
    if (!e.valid) return false;
    return use(e.freesyncMin, e.freesyncMax, "EDID FreeSync data block") ||
           use(e.adaptiveMin, e.adaptiveMax, "EDID DisplayID Adaptive-Sync block") ||
           use(e.hdmiVrrMin, e.hdmiVrrMax > 0 ? e.hdmiVrrMax : e.rangeMax, "EDID HDMI VRR range") ||
           use(e.rangeMin, e.rangeMax, "EDID range limits");
}

std::string edidMonitorKey(const EdidInfo& e) {
    if (!e.valid) return {};
    char buf[96];
    std::snprintf(buf, sizeof buf, "%s-%04X-%s", e.pnpId.c_str(), e.product,
                  !e.serialText.empty() ? e.serialText.c_str() : std::to_string(e.serial).c_str());
    std::string s = buf;
    for (char& c : s)
        if (c == ' ' || c == '\t') c = '_';
    return s;
}

}  // namespace vrr
