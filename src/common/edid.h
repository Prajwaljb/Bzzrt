// EDID parsing for what the check needs: the monitor's identity and every place a
// VRR range can be declared (FreeSync data block, DisplayID Adaptive-Sync, HDMI VRR,
// and the base block's range limits).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrr {

struct EdidInfo {
    bool valid = false;
    std::string pnpId;       // manufacturer, e.g. "BNQ"
    uint16_t product = 0;
    uint32_t serial = 0;
    std::string name;        // 0xFC display product name
    std::string serialText;  // 0xFF display serial number
    // 0 = not present. Ranges in Hz.
    double rangeMin = 0, rangeMax = 0;          // base block display range limits (0xFD)
    double freesyncMin = 0, freesyncMax = 0;    // AMD vendor-specific data block (FreeSync)
    double adaptiveMin = 0, adaptiveMax = 0;    // DisplayID 2.x Adaptive-Sync data block
    double hdmiVrrMin = 0, hdmiVrrMax = 0;      // HDMI Forum VRRmin / VRRmax
};

EdidInfo parseEdid(const std::vector<uint8_t>& edid);

// The most specific VRR range the EDID declares. False if it declares none that looks like one.
bool edidVrrRange(const EdidInfo& e, double& minHz, double& maxHz, std::string& source);

// Stable identity for remembering a monitor's range, e.g. "BNQ-7F5A-1234ABCD".
std::string edidMonitorKey(const EdidInfo& e);

}  // namespace vrr
