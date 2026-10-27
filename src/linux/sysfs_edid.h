// Reads a connector's EDID from /sys/class/drm (available without DRM master).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vrr {

// connector: kernel connector name such as "DP-2" or "HDMI-A-1". Empty result if not found. If no connector
// matches and exactly one external display is connected, that one's EDID is returned.
std::vector<uint8_t> readSysfsEdid(const std::string& connector);

}  // namespace vrr
