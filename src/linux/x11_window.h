// X11 (XCB) window hosting a Vulkan surface.
#pragma once

#include <xcb/xcb.h>

#include <string>
#include <vector>

#include "../common/config.h"
#include "vk_window.h"

namespace vrr {

class X11Window : public VkWindow {
public:
    ~X11Window() override;
    bool init(const Config& cfg, std::string& err);

    const char* name() const override { return "X11"; }
    void pollEvents(std::vector<KeyEvent>& keys) override;
    bool quitRequested() const override { return closed_; }
    std::vector<WindowMode> supportedWindowModes() const override {
        return {WindowMode::Windowed, WindowMode::Borderless};
    }
    WindowMode windowMode() const override { return mode_; }
    void setWindowMode(WindowMode m) override;
    DisplayInfo displayInfo() override;

    const char* surfaceExtension() const override;
    VkResult createSurface(VkInstance instance, VkSurfaceKHR* surface) override;
    void extent(uint32_t& w, uint32_t& h) const override {
        w = width_;
        h = height_;
    }

private:
    xcb_atom_t atom(const char* name);
    void queryDisplay();

    xcb_connection_t* conn_ = nullptr;
    xcb_screen_t* screen_ = nullptr;
    xcb_window_t window_ = 0;
    xcb_atom_t wmProtocols_ = 0, wmDelete_ = 0, netWmState_ = 0, netWmStateFullscreen_ = 0;
    uint32_t width_ = 0, height_ = 0;
    bool closed_ = false;
    bool displayDirty_ = true;
    WindowMode mode_ = WindowMode::Borderless;
    DisplayInfo cached_;
};

}  // namespace vrr
