// Command-line configuration.
#pragma once

#include <string>

#include "backend.h"
#include "scheduler.h"

namespace vrr {

struct Config {
    std::string api;           // d3d12 | d3d11 | vulkan | kms   (empty = platform default)
    std::string wsi = "auto";  // auto | wayland | x11           (Linux Vulkan)
    int gpuIndex = -1;
    WindowMode windowMode = WindowMode::Borderless;
    int windowW = 1280, windowH = 720;
    bool tenBit = false;
    std::string presentMode;   // backend specific id, e.g. fifo, immediate, vsync, tearing, vrr
    std::string test = "stable100";
    bool autostart = false;
    bool advanced = false;     // start in the advanced test bench instead of the guided check
    double durationSec = 60.0; // 0 = until stopped
    bool exitAfter = false;
    bool log = true;
    std::string logDir = ".";
    int pattern = kPatMidGray;
    bool patternGiven = false;
    std::string imagePath;
    bool overlay = true;
    bool frameCounter = false;
    double idleFps = 100.0;
    double vrrMinHz = 48.0;
    double vrrMaxHz = 200.0;
    TestParams params;
    std::string kmsDevice;     // /dev/dri/cardN
    std::string kmsConnector;  // e.g. DP-2
    std::string kmsMode;       // WxH@Hz
    bool debug = false;
    bool listTests = false;
    bool help = false;
};

// Returns false on error (message in err).
bool parseArgs(int argc, char** argv, Config& cfg, std::string& err);
void printUsage(const char* argv0);
void printTests();

}  // namespace vrr
