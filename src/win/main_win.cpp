// Windows entry point: Direct3D 12 (default) or Direct3D 11 on a DXGI flip-model swap chain.
#include <windows.h>

#include <cstdio>
#include <memory>
#include <string>

#include "../common/app.h"
#include "../common/clock.h"
#include "../common/config.h"
#include "../common/image_load.h"
#include "d3d12_backend.h"
#ifdef VRRDIAG_HAVE_D3D11
#include "d3d11_backend.h"
#endif
#include "win_platform.h"

using namespace vrr;

int main(int argc, char** argv) {
    // Per-monitor DPI awareness: otherwise a "fullscreen" window may be DPI-scaled and lose independent flip.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

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
        std::printf("Bzzrt %s\n", kVersion);
        return 0;
    }
    if (cfg.listTests) {
        printTests();
        return 0;
    }
    const bool apiExplicit = !cfg.api.empty();
    if (!apiExplicit) cfg.api = "d3d12";
    if (cfg.api != "d3d12" && cfg.api != "d3d11") {
        std::fprintf(stderr, "Unknown --api '%s' on Windows (use d3d12 or d3d11).\n", cfg.api.c_str());
        return 2;
    }

    ImageData image;
    if (!cfg.imagePath.empty() && !loadImage(cfg.imagePath, image, err))
        std::fprintf(stderr, "Warning: cannot load image: %s\n", err.c_str());

    initTimingPrecision();
    WinPlatform window;
    if (!window.create(cfg, err)) {
        std::fprintf(stderr, "Window: %s\n", err.c_str());
        return 1;
    }

    std::unique_ptr<DxgiBackendBase> backend;
    if (cfg.api == "d3d12") {
        auto b = std::make_unique<D3D12Backend>();
        if (b->init(window.hwnd(), cfg, image, err)) {
            backend = std::move(b);
        } else {
            std::fprintf(stderr, "Direct3D 12: %s\n", err.c_str());
        }
    }
#ifdef VRRDIAG_HAVE_D3D11
    if (!backend && (cfg.api == "d3d11" || !apiExplicit)) {
        if (cfg.api == "d3d12") std::fprintf(stderr, "Falling back to Direct3D 11.\n");
        auto b = std::make_unique<D3D11Backend>();
        if (b->init(window.hwnd(), cfg, image, err)) {
            backend = std::move(b);
        } else {
            std::fprintf(stderr, "Direct3D 11: %s\n", err.c_str());
        }
    }
#endif
    if (!backend) {
        std::fprintf(stderr, "No usable graphics backend.\n");
        return 1;
    }

    window.setBackend(backend.get());
    window.setWindowMode(cfg.windowMode);
    int rc = 0;
    {
        App app(cfg, window, *backend);
        rc = app.run();
    }
    window.setWindowMode(WindowMode::Windowed);
    backend.reset();
    shutdownTimingPrecision();
    return rc;
}
