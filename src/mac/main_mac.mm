// macOS entry point: Metal on a CAMetalLayer (Apple silicon and Intel Macs, macOS 12+).
#import <AppKit/AppKit.h>

#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "../common/app.h"
#include "../common/clock.h"
#include "../common/config.h"
#include "../common/image_load.h"
#include "mac_platform.h"
#include "metal_backend.h"

using namespace vrr;

int main(int argc, char** argv) {
    @autoreleasepool {
        Config cfg;
        std::string err;
        if (!parseArgs(argc, argv, cfg, err)) {
            std::fprintf(stderr, "Error: %s\n\n", err.c_str());
            printUsage(argv[0]);
            return 2;
        }
        if (cfg.help) {
            printUsage(argv[0]);
            return 0;
        }
        if (cfg.version) {
            std::printf("Synchora %s\n", kVersion);
            return 0;
        }
        if (cfg.listTests) {
            printTests();
            return 0;
        }
        if (!cfg.api.empty() && cfg.api != "metal") {
            std::fprintf(stderr, "Unknown --api '%s' on macOS (use metal).\n", cfg.api.c_str());
            return 2;
        }
        cfg.api = "metal";
        // Started from Finder the working directory is "/", so logs go to ~/Documents/Synchora instead.
        if (cfg.logDir == "." && access(".", W_OK) != 0) {
            if (const char* home = std::getenv("HOME")) {
                cfg.logDir = std::string(home) + "/Documents/Synchora";
                std::error_code ec;
                std::filesystem::create_directories(cfg.logDir, ec);
            }
        }

        ImageData image;
        if (!cfg.imagePath.empty() && !loadImage(cfg.imagePath, image, err))
            std::fprintf(stderr, "Warning: cannot load image: %s\n", err.c_str());

        initTimingPrecision();
        MacPlatform window;
        if (!window.create(cfg, err)) {
            std::fprintf(stderr, "Window: %s\n", err.c_str());
            return 1;
        }
        MetalBackend metal;
        if (!metal.init(window, cfg, image, err)) {
            std::fprintf(stderr, "Metal: %s\n", err.c_str());
            return 1;
        }
        window.setWindowMode(cfg.windowMode);
        int rc = 0;
        {
            App app(cfg, window, metal);
            rc = app.run();
        }
        window.setWindowMode(WindowMode::Windowed);
        shutdownTimingPrecision();
        return rc;
    }
}
