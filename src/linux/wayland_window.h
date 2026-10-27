// Wayland (xdg-shell) window hosting a Vulkan surface.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "../common/config.h"
#include "vk_window.h"

struct wl_display;
struct wl_registry;
struct wl_compositor;
struct wl_surface;
struct wl_seat;
struct wl_keyboard;
struct wl_pointer;
struct wl_output;
struct xdg_wm_base;
struct xdg_surface;
struct xdg_toplevel;
struct wp_content_type_manager_v1;
struct wp_content_type_v1;

namespace vrr {

class WaylandWindow : public VkWindow {
public:
    ~WaylandWindow() override;
    bool init(const Config& cfg, std::string& err);

    const char* name() const override { return "Wayland"; }
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
        w = static_cast<uint32_t>(width_);
        h = static_cast<uint32_t>(height_);
    }

    struct Output {
        wl_output* output = nullptr;
        uint32_t globalName = 0;
        std::string name, description, make, model;
        int width = 0, height = 0;
        int refreshMhz = 0;
    };

    // State is public so the C listener callbacks in the .cpp can update it.
    wl_display* display_ = nullptr;
    wl_registry* registry_ = nullptr;
    wl_compositor* compositor_ = nullptr;
    xdg_wm_base* wmBase_ = nullptr;
    wl_seat* seat_ = nullptr;
    wl_keyboard* keyboard_ = nullptr;
    wl_pointer* pointer_ = nullptr;
    wl_surface* surface_ = nullptr;
    xdg_surface* xdgSurface_ = nullptr;
    xdg_toplevel* toplevel_ = nullptr;
    wp_content_type_manager_v1* contentTypeMgr_ = nullptr;
    wp_content_type_v1* contentType_ = nullptr;
    std::vector<std::unique_ptr<Output>> outputs_;
    Output* currentOutput_ = nullptr;

    int width_ = 0, height_ = 0;
    int pendingW_ = 0, pendingH_ = 0;
    int windowedW_ = 1280, windowedH_ = 720;
    bool configured_ = false;
    bool closed_ = false;
    bool shift_ = false;
    WindowMode mode_ = WindowMode::Borderless;
    std::vector<KeyEvent> pendingKeys_;
    std::string edidConnector_;  // EDID cache: read again only when the window moves to another output
    std::vector<uint8_t> edid_;
    bool edidRead_ = false;
};

}  // namespace vrr
