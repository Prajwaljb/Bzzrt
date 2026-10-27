// Linux entry point: Vulkan WSI (Wayland / X11) or direct DRM/KMS.
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>

#include "../common/app.h"
#include "../common/clock.h"
#include "../common/config.h"
#include "../common/image_load.h"
#include "vk_backend.h"
#ifdef VRRDIAG_HAVE_WAYLAND
#include "wayland_window.h"
#endif
#ifdef VRRDIAG_HAVE_X11
#include "x11_window.h"
#endif
#ifdef VRRDIAG_HAVE_KMS
#include "kms_backend.h"
#endif

using namespace vrr;

int main(int argc, char** argv) {
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
    if (cfg.api.empty()) cfg.api = "vulkan";

    ImageData image;
    if (!cfg.imagePath.empty() && !loadImage(cfg.imagePath, image, err))
        std::fprintf(stderr, "Warning: cannot load image: %s\n", err.c_str());

    initTimingPrecision();

    if (cfg.api == "kms") {
#ifdef VRRDIAG_HAVE_KMS
        KmsBackend kms;
        if (!kms.init(cfg, image, err)) {
            std::fprintf(stderr, "KMS backend: %s\n", err.c_str());
            return 1;
        }
        App app(cfg, kms, kms);
        return app.run();
#else
        std::fprintf(stderr, "This build has no DRM/KMS backend (libdrm not found at build time).\n");
        return 1;
#endif
    }
    if (cfg.api != "vulkan") {
        std::fprintf(stderr, "Unknown --api '%s' on Linux (use vulkan or kms).\n", cfg.api.c_str());
        return 2;
    }

    std::string wsi = cfg.wsi;
    if (wsi == "auto") {
        const char* wl = std::getenv("WAYLAND_DISPLAY");
        wsi = (wl && *wl) ? "wayland" : "x11";
    }
    std::unique_ptr<VkWindow> window;
#ifdef VRRDIAG_HAVE_WAYLAND
    if (wsi == "wayland") {
        auto w = std::make_unique<WaylandWindow>();
        if (!w->init(cfg, err)) {
            std::fprintf(stderr, "Wayland: %s\n", err.c_str());
            return 1;
        }
        window = std::move(w);
    }
#endif
#ifdef VRRDIAG_HAVE_X11
    if (wsi == "x11") {
        auto w = std::make_unique<X11Window>();
        if (!w->init(cfg, err)) {
            std::fprintf(stderr, "X11: %s\n", err.c_str());
            return 1;
        }
        window = std::move(w);
    }
#endif
    if (!window) {
        std::fprintf(stderr, "WSI '%s' is not available in this build.\n", wsi.c_str());
        return 1;
    }

    VulkanBackend vk(*window);
    if (!vk.init(cfg, image, err)) {
        std::fprintf(stderr, "Vulkan: %s\n", err.c_str());
        return 1;
    }
    App app(cfg, *window, vk);
    const int rc = app.run();
    shutdownTimingPrecision();
    return rc;
}
