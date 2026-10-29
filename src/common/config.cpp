#include "config.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vrr {

void printUsage(const char* argv0) {
    std::printf(
        "Bzzrt %s - VRR / FreeSync / G-SYNC / Adaptive-Sync flicker and LFC diagnostic\n\n"
        "Usage: %s [options]\n\n"
        "Backend\n"
#if defined(_WIN32)
        "  --api d3d12|d3d11          Graphics API (default d3d12, falls back to d3d11)\n"
#elif defined(__APPLE__)
        "  --api metal                Graphics API (Metal, the only one on macOS)\n"
#else
        "  --api vulkan|kms           vulkan = Vulkan WSI under a compositor (default)\n"
        "                             kms    = direct DRM/KMS page flips from a text console\n"
        "  --wsi auto|wayland|x11     Vulkan window system (default auto)\n"
        "  --kms-device /dev/dri/cardN   --kms-connector DP-2   --kms-mode 2560x1440@200\n"
#endif
        "  --double-frames            Experiment: the app itself shows frames below the LFC threshold\n"
        "                             several times at even intervals, as a driver with LFC would\n"
#ifdef _WIN32
        "  --gpu N                    GPU / adapter index (default: the GPU driving the monitor)\n"
#else
        "  --gpu N                    GPU / adapter index (default: first usable)\n"
#endif
        "  --present ID               Present mode id (see overlay; e.g. fifo, mailbox, immediate,\n"
        "                             vsync, tearing, nosync, vrr, fixed, async)\n"
        "  --10bit                    Request a 10-bit swap chain (SDR, no HDR)\n"
        "  --windowed | --borderless"
#ifdef _WIN32
        " | --exclusive"
#endif
        "\n"
        "  --size WxH                 Windowed size (default 1280x720)\n"
        "  --debug                    Enable API validation layers\n\n"
        "Mode\n"
        "  (default)                  Guided flicker check: step-by-step, answer Y/N, get a verdict\n"
        "  --check                    Start the guided check right away (no ENTER needed)\n"
        "  --advanced                 Advanced test bench (implied by --test, --fps and --start)\n\n"
        "Test\n"
        "  --test ID                  Test id (see --list-tests; default stable100)\n"
        "  --fps F                    Custom stable FPS (selects test 'custom')\n"
        "  --start                    Start the test immediately\n"
        "  --duration S               Test duration in seconds, 0 = until stopped (default 60)\n"
        "  --exit-after               Quit when the test finishes\n"
        "  --seed N                   Random seed for jitter tests (default 1)\n"
        "  --idle-fps F               Rate used while no test runs (default 100)\n"
        "  --vrr-min HZ --vrr-max HZ  Monitor VRR range (default: read from the monitor's EDID / the OS;\n"
        "                             if it cannot be read, the check asks for it and remembers it)\n"
        "  --lfc-threshold FPS        Frame rate below which frames are expected to be shown several\n"
        "                             times (default: 1.1 x the VRR minimum)\n"
        "  --switch-period S          Time at each rate in switch tests (default 1.0)\n"
        "  --sweep-period S           Sweep cycle length (default 10)\n"
        "  --spike-rate HZ            Spikes per second (default 0.5)\n"
        "  --spike-min MS --spike-max MS   Spike frame time range (default 25..50)\n"
        "  --spike-frames N           Consecutive slow frames per spike (default 1)\n"
        "  --drop-rate HZ --drop-duration S --drop-min FPS --drop-max FPS\n"
        "  --jitter-min FPS --jitter-max FPS\n"
        "  --lows-avg FPS --lows-fps FPS --lows-percent P   (test 'lows')\n\n"
        "Display\n"
        "  --pattern N                0 mid-gray, 1 dark-gray, 2 white, 3 black, 4 gradient,\n"
        "                             5 high-contrast, 6 moving, 7 image\n"
        "  --image FILE               Image for pattern 7 (24/32-bit BMP or binary PPM)\n"
        "  --no-overlay               Start with the statistics overlay hidden\n"
        "  --counter                  Show the binary frame counter (for high-speed cameras)\n\n"
        "Logging\n"
        "  --no-log                   Do not write CSV logs\n"
        "  --log-dir DIR              Directory for CSV logs (default .)\n\n"
        "  --list-tests  --version  --help\n",
        kVersion, argv0);
}

void printTests() {
    std::printf("Available tests (--test ID):\n");
    for (const auto& t : allTests()) std::printf("  %-14s %s\n", t.id.c_str(), t.name.c_str());
}

static bool parseSize(const char* s, int& w, int& h) { return std::sscanf(s, "%dx%d", &w, &h) == 2 && w > 0 && h > 0; }

bool parseArgs(int argc, char** argv, Config& c, std::string& err) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char*& v) -> bool {
            if (i + 1 >= argc) {
                err = "missing value for " + a;
                return false;
            }
            v = argv[++i];
            return true;
        };
        auto num = [&](double& out) -> bool {
            const char* v;
            if (!need(v)) return false;
            char* end = nullptr;
            out = std::strtod(v, &end);
            if (end == v || *end || !std::isfinite(out)) {
                err = "bad number for " + a + ": " + v;
                return false;
            }
            return true;
        };
        auto str = [&](std::string& out) -> bool {
            const char* v;
            if (!need(v)) return false;
            out = v;
            return true;
        };
        double d = 0;
        if (a == "--help" || a == "-h") c.help = true;
        else if (a == "--list-tests") c.listTests = true;
        else if (a == "--version" || a == "-V") c.version = true;
        else if (a == "--api") { if (!str(c.api)) return false; }
        else if (a == "--wsi") { if (!str(c.wsi)) return false; }
        else if (a == "--gpu") { if (!num(d)) return false; if (d < 0 || d > 64) return err = "bad --gpu", false; c.gpuIndex = static_cast<int>(d); }
        else if (a == "--present") { if (!str(c.presentMode)) return false; }
        else if (a == "--10bit") c.tenBit = true;
        else if (a == "--8bit") c.tenBit = false;
        else if (a == "--windowed") c.windowMode = WindowMode::Windowed;
        else if (a == "--borderless" || a == "--fullscreen") c.windowMode = WindowMode::Borderless;
        else if (a == "--exclusive") c.windowMode = WindowMode::Exclusive;
        else if (a == "--size") {
            const char* v;
            if (!need(v)) return false;
            if (!parseSize(v, c.windowW, c.windowH)) return err = "bad --size", false;
        }
        else if (a == "--debug") c.debug = true;
        else if (a == "--advanced") c.advanced = true;
        else if (a == "--check") c.autostart = true;
        else if (a == "--test") { if (!str(c.test)) return false; c.advanced = true; }
        else if (a == "--fps") { if (!num(c.params.customFps)) return false; c.test = "custom"; c.advanced = true; }
        else if (a == "--start") { c.autostart = true; c.advanced = true; }
        else if (a == "--duration") { if (!num(c.durationSec)) return false; }
        else if (a == "--exit-after") c.exitAfter = true;
        else if (a == "--seed") { if (!num(d)) return false; if (d < 0 || d >= 1.8e19) return err = "bad --seed", false; c.params.seed = static_cast<uint64_t>(d); }
        else if (a == "--idle-fps") { if (!num(c.idleFps)) return false; }
        else if (a == "--vrr-min") { if (!num(c.vrrMinHz)) return false; }
        else if (a == "--vrr-max") { if (!num(c.vrrMaxHz)) return false; }
        else if (a == "--lfc-threshold") { if (!num(c.lfcFps)) return false; }
        else if (a == "--switch-period") { if (!num(c.params.switchPeriodSec)) return false; }
        else if (a == "--sweep-period") { if (!num(c.params.sweepPeriodSec)) return false; }
        else if (a == "--spike-rate") { if (!num(c.params.spikeRateHz)) return false; }
        else if (a == "--spike-min") { if (!num(c.params.spikeMinMs)) return false; }
        else if (a == "--spike-max") { if (!num(c.params.spikeMaxMs)) return false; }
        else if (a == "--spike-frames") { if (!num(d)) return false; if (d < 1 || d > 1000) return err = "bad --spike-frames", false; c.params.spikeFrames = static_cast<int>(d); }
        else if (a == "--drop-rate") { if (!num(c.params.dropRateHz)) return false; }
        else if (a == "--drop-duration") { if (!num(c.params.dropDurationSec)) return false; }
        else if (a == "--drop-min") { if (!num(c.params.dropMinFps)) return false; }
        else if (a == "--drop-max") { if (!num(c.params.dropMaxFps)) return false; }
        else if (a == "--jitter-min") { if (!num(c.params.jitterMinFps)) return false; }
        else if (a == "--jitter-max") { if (!num(c.params.jitterMaxFps)) return false; }
        else if (a == "--lows-avg") { if (!num(c.params.lowsAvgFps)) return false; }
        else if (a == "--lows-fps") { if (!num(c.params.lowsFps)) return false; }
        else if (a == "--lows-percent") { if (!num(c.params.lowsPercent)) return false; }
        else if (a == "--pattern") { if (!num(d)) return false; c.pattern = d >= 0 && d < kPatCount ? static_cast<int>(d) : -1; c.patternGiven = true; }
        else if (a == "--image") { if (!str(c.imagePath)) return false; }
        else if (a == "--no-overlay") c.overlay = false;
        else if (a == "--counter") c.frameCounter = true;
        else if (a == "--no-log") c.log = false;
        else if (a == "--log-dir") { if (!str(c.logDir)) return false; }
        else if (a == "--kms-device") { if (!str(c.kmsDevice)) return false; }
        else if (a == "--kms-connector") { if (!str(c.kmsConnector)) return false; }
        else if (a == "--kms-mode") { if (!str(c.kmsMode)) return false; }
        else if (a == "--double-frames") c.doubleFrames = true;
        else {
            err = "unknown option: " + a;
            return false;
        }
    }
    if (findTest(c.test) < 0) return err = "unknown test id: " + c.test + " (see --list-tests)", false;
    if (c.pattern < 0 || c.pattern >= kPatCount) return err = "bad --pattern", false;
    if (c.vrrMinHz < 0 || c.vrrMaxHz < 0 || c.vrrMinHz > 1000 || c.vrrMaxHz > 1000)
        return err = "--vrr-min and --vrr-max must be between 1 and 1000 Hz", false;
    if (c.vrrMinHz > 0 && c.vrrMaxHz > 0 && c.vrrMaxHz <= c.vrrMinHz)
        return err = "bad VRR range (--vrr-min must be below --vrr-max)", false;
    if (c.lfcFps < 0 || c.lfcFps > 1000) return err = "bad --lfc-threshold", false;
    if (c.idleFps <= 0) return err = "bad --idle-fps", false;
    if (c.durationSec < 0) return err = "bad --duration", false;
    if (c.params.customFps <= 0 || c.params.lowsAvgFps <= 0 || c.params.lowsFps <= 0)
        return err = "FPS values must be positive", false;
    if (c.params.spikeMinMs <= 0 || c.params.spikeMaxMs < c.params.spikeMinMs) return err = "bad spike range", false;
    if (c.params.dropMinFps <= 0 || c.params.dropMaxFps < c.params.dropMinFps) return err = "bad drop range", false;
    if (c.params.jitterMinFps <= 0 || c.params.jitterMaxFps < c.params.jitterMinFps)
        return err = "bad jitter range", false;
    return true;
}

}  // namespace vrr
