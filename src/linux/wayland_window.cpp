#include "wayland_window.h"

#include <linux/input-event-codes.h>
#include <poll.h>
#include <unistd.h>
#include <wayland-client.h>

#include <cstdio>
#include <cstring>

#include "sysfs_edid.h"
#include "xdg-shell-client-protocol.h"
#ifdef VRRDIAG_HAVE_CONTENT_TYPE
#include "content-type-v1-client-protocol.h"
#endif

#include <vulkan/vulkan_wayland.h>

namespace vrr {

namespace {

WaylandWindow* self(void* data) { return static_cast<WaylandWindow*>(data); }

// ---- wl_output ----
void outGeometry(void* data, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char* make,
                 const char* model, int32_t) {
    auto* o = static_cast<WaylandWindow::Output*>(data);
    o->make = make ? make : "";
    o->model = model ? model : "";
}
void outMode(void* data, wl_output*, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    auto* o = static_cast<WaylandWindow::Output*>(data);
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        o->width = w;
        o->height = h;
        o->refreshMhz = refresh;
    }
}
void outDone(void*, wl_output*) {}
void outScale(void*, wl_output*, int32_t) {}
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
void outName(void* data, wl_output*, const char* name) { static_cast<WaylandWindow::Output*>(data)->name = name; }
void outDescription(void* data, wl_output*, const char* d) {
    static_cast<WaylandWindow::Output*>(data)->description = d;
}
#endif
const wl_output_listener kOutputListener = {
    outGeometry, outMode, outDone, outScale,
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
    outName, outDescription,
#endif
};

// ---- wl_surface ----
void surfEnter(void* data, wl_surface*, wl_output* output) {
    for (auto& o : self(data)->outputs_)
        if (o->output == output) self(data)->currentOutput_ = o.get();
}
void surfLeave(void*, wl_surface*, wl_output*) {}
const wl_surface_listener kSurfaceListener = {surfEnter, surfLeave};

// ---- xdg ----
void wmPing(void*, xdg_wm_base* base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
const xdg_wm_base_listener kWmBaseListener = {wmPing};

void xdgSurfaceConfigure(void* data, xdg_surface* s, uint32_t serial) {
    WaylandWindow* w = self(data);
    xdg_surface_ack_configure(s, serial);
    if (w->pendingW_ > 0 && w->pendingH_ > 0) {
        w->width_ = w->pendingW_;
        w->height_ = w->pendingH_;
    } else {
        w->width_ = w->windowedW_;
        w->height_ = w->windowedH_;
    }
    w->configured_ = true;
}
const xdg_surface_listener kXdgSurfaceListener = {xdgSurfaceConfigure};

void toplevelConfigure(void* data, xdg_toplevel*, int32_t width, int32_t height, wl_array*) {
    self(data)->pendingW_ = width;
    self(data)->pendingH_ = height;
}
void toplevelClose(void* data, xdg_toplevel*) { self(data)->closed_ = true; }
const xdg_toplevel_listener kToplevelListener = {toplevelConfigure, toplevelClose};

// ---- input ----
void kbKeymap(void*, wl_keyboard*, uint32_t, int32_t fd, uint32_t) { close(fd); }
void kbEnter(void*, wl_keyboard*, uint32_t, wl_surface*, wl_array*) {}
void kbLeave(void* data, wl_keyboard*, uint32_t, wl_surface*) { self(data)->shift_ = false; }
void kbKey(void* data, wl_keyboard*, uint32_t, uint32_t, uint32_t key, uint32_t state) {
    WaylandWindow* w = self(data);
    const bool down = state == WL_KEYBOARD_KEY_STATE_PRESSED;
    if (key == KEY_LEFTSHIFT || key == KEY_RIGHTSHIFT) {
        w->shift_ = down;
        return;
    }
    if (!down) return;
    const Key k = keyFromEvdev(key);
    if (k != Key::None) w->pendingKeys_.push_back({k, w->shift_});
}
void kbModifiers(void*, wl_keyboard*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t) {}
void kbRepeat(void*, wl_keyboard*, int32_t, int32_t) {}
const wl_keyboard_listener kKeyboardListener = {kbKeymap, kbEnter, kbLeave, kbKey, kbModifiers, kbRepeat};

void ptrEnter(void*, wl_pointer* p, uint32_t serial, wl_surface*, wl_fixed_t, wl_fixed_t) {
    // Hide the cursor: cursor plane updates can force extra refreshes on some compositors.
    wl_pointer_set_cursor(p, serial, nullptr, 0, 0);
}
void ptrLeave(void*, wl_pointer*, uint32_t, wl_surface*) {}
void ptrMotion(void*, wl_pointer*, uint32_t, wl_fixed_t, wl_fixed_t) {}
void ptrButton(void*, wl_pointer*, uint32_t, uint32_t, uint32_t, uint32_t) {}
void ptrAxis(void*, wl_pointer*, uint32_t, uint32_t, wl_fixed_t) {}
void ptrFrame(void*, wl_pointer*) {}
void ptrAxisSource(void*, wl_pointer*, uint32_t) {}
void ptrAxisStop(void*, wl_pointer*, uint32_t, uint32_t) {}
void ptrAxisDiscrete(void*, wl_pointer*, uint32_t, int32_t) {}
const wl_pointer_listener kPointerListener = {ptrEnter, ptrLeave,      ptrMotion,   ptrButton,      ptrAxis,
                                              ptrFrame, ptrAxisSource, ptrAxisStop, ptrAxisDiscrete};

void seatCaps(void* data, wl_seat* seat, uint32_t caps) {
    WaylandWindow* w = self(data);
    if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !w->keyboard_) {
        w->keyboard_ = wl_seat_get_keyboard(seat);
        wl_keyboard_add_listener(w->keyboard_, &kKeyboardListener, w);
    }
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !w->pointer_) {
        w->pointer_ = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(w->pointer_, &kPointerListener, w);
    }
}
void seatName(void*, wl_seat*, const char*) {}
const wl_seat_listener kSeatListener = {seatCaps, seatName};

// ---- registry ----
void regGlobal(void* data, wl_registry* reg, uint32_t name, const char* iface, uint32_t version) {
    WaylandWindow* w = self(data);
    auto minv = [](uint32_t a, uint32_t b) { return a < b ? a : b; };
    if (!std::strcmp(iface, wl_compositor_interface.name)) {
        w->compositor_ = static_cast<wl_compositor*>(wl_registry_bind(reg, name, &wl_compositor_interface, minv(version, 4)));
    } else if (!std::strcmp(iface, xdg_wm_base_interface.name)) {
        w->wmBase_ = static_cast<xdg_wm_base*>(wl_registry_bind(reg, name, &xdg_wm_base_interface, 1));
        xdg_wm_base_add_listener(w->wmBase_, &kWmBaseListener, w);
    } else if (!std::strcmp(iface, wl_seat_interface.name) && !w->seat_) {
        w->seat_ = static_cast<wl_seat*>(wl_registry_bind(reg, name, &wl_seat_interface, minv(version, 5)));
        wl_seat_add_listener(w->seat_, &kSeatListener, w);
    } else if (!std::strcmp(iface, wl_output_interface.name)) {
        auto o = std::make_unique<WaylandWindow::Output>();
#ifdef WL_OUTPUT_NAME_SINCE_VERSION
        const uint32_t v = minv(version, 4);
#else
        const uint32_t v = minv(version, 2);
#endif
        o->output = static_cast<wl_output*>(wl_registry_bind(reg, name, &wl_output_interface, v));
        o->globalName = name;
        wl_output_add_listener(o->output, &kOutputListener, o.get());
        w->outputs_.push_back(std::move(o));
    }
#ifdef VRRDIAG_HAVE_CONTENT_TYPE
    else if (!std::strcmp(iface, wp_content_type_manager_v1_interface.name)) {
        w->contentTypeMgr_ = static_cast<wp_content_type_manager_v1*>(
            wl_registry_bind(reg, name, &wp_content_type_manager_v1_interface, 1));
    }
#endif
}
void regRemove(void* data, wl_registry*, uint32_t name) {
    WaylandWindow* w = self(data);
    for (auto it = w->outputs_.begin(); it != w->outputs_.end(); ++it) {
        if ((*it)->globalName == name) {
            if (w->currentOutput_ == it->get()) w->currentOutput_ = nullptr;
            wl_output_destroy((*it)->output);
            w->outputs_.erase(it);
            break;
        }
    }
}
const wl_registry_listener kRegistryListener = {regGlobal, regRemove};

}  // namespace

WaylandWindow::~WaylandWindow() {
#ifdef VRRDIAG_HAVE_CONTENT_TYPE
    if (contentType_) wp_content_type_v1_destroy(contentType_);
    if (contentTypeMgr_) wp_content_type_manager_v1_destroy(contentTypeMgr_);
#endif
    if (toplevel_) xdg_toplevel_destroy(toplevel_);
    if (xdgSurface_) xdg_surface_destroy(xdgSurface_);
    if (surface_) wl_surface_destroy(surface_);
    if (keyboard_) wl_keyboard_destroy(keyboard_);
    if (pointer_) wl_pointer_destroy(pointer_);
    if (seat_) wl_seat_destroy(seat_);
    for (auto& o : outputs_) wl_output_destroy(o->output);
    if (wmBase_) xdg_wm_base_destroy(wmBase_);
    if (compositor_) wl_compositor_destroy(compositor_);
    if (registry_) wl_registry_destroy(registry_);
    if (display_) wl_display_disconnect(display_);
}

bool WaylandWindow::init(const Config& cfg, std::string& err) {
    windowedW_ = cfg.windowW;
    windowedH_ = cfg.windowH;
    mode_ = cfg.windowMode == WindowMode::Windowed ? WindowMode::Windowed : WindowMode::Borderless;
    display_ = wl_display_connect(nullptr);
    if (!display_) return err = "cannot connect to Wayland display (WAYLAND_DISPLAY)", false;
    registry_ = wl_display_get_registry(display_);
    wl_registry_add_listener(registry_, &kRegistryListener, this);
    wl_display_roundtrip(display_);
    wl_display_roundtrip(display_);  // output / seat events
    if (!compositor_ || !wmBase_) return err = "compositor lacks wl_compositor or xdg_wm_base", false;

    surface_ = wl_compositor_create_surface(compositor_);
    wl_surface_add_listener(surface_, &kSurfaceListener, this);
    xdgSurface_ = xdg_wm_base_get_xdg_surface(wmBase_, surface_);
    xdg_surface_add_listener(xdgSurface_, &kXdgSurfaceListener, this);
    toplevel_ = xdg_surface_get_toplevel(xdgSurface_);
    xdg_toplevel_add_listener(toplevel_, &kToplevelListener, this);
    xdg_toplevel_set_title(toplevel_, "Synchora - VRR flicker check");
    xdg_toplevel_set_app_id(toplevel_, "vrrdiag");
    if (mode_ == WindowMode::Borderless) xdg_toplevel_set_fullscreen(toplevel_, nullptr);
#ifdef VRRDIAG_HAVE_CONTENT_TYPE
    if (contentTypeMgr_) {
        contentType_ = wp_content_type_manager_v1_get_surface_content_type(contentTypeMgr_, surface_);
        wp_content_type_v1_set_content_type(contentType_, WP_CONTENT_TYPE_V1_TYPE_GAME);
    }
#endif
    wl_surface_commit(surface_);
    while (!configured_ && !closed_) {
        if (wl_display_dispatch(display_) < 0) return err = "Wayland connection lost", false;
    }
    if (!currentOutput_ && !outputs_.empty()) currentOutput_ = outputs_.front().get();
    return true;
}

void WaylandWindow::pollEvents(std::vector<KeyEvent>& keys) {
    while (wl_display_prepare_read(display_) != 0) wl_display_dispatch_pending(display_);
    wl_display_flush(display_);
    pollfd pfd{wl_display_get_fd(display_), POLLIN, 0};
    if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
        wl_display_read_events(display_);
    } else {
        wl_display_cancel_read(display_);
    }
    if (wl_display_dispatch_pending(display_) < 0) closed_ = true;
    keys.insert(keys.end(), pendingKeys_.begin(), pendingKeys_.end());
    pendingKeys_.clear();
}

void WaylandWindow::setWindowMode(WindowMode m) {
    if (m == WindowMode::Exclusive) m = WindowMode::Borderless;
    if (m == mode_) return;
    mode_ = m;
    if (m == WindowMode::Borderless) xdg_toplevel_set_fullscreen(toplevel_, nullptr);
    else xdg_toplevel_unset_fullscreen(toplevel_);
    wl_surface_commit(surface_);
    wl_display_flush(display_);
}

DisplayInfo WaylandWindow::displayInfo() {
    DisplayInfo d;
    const Output* o = currentOutput_ ? currentOutput_ : (outputs_.empty() ? nullptr : outputs_.front().get());
    if (o) {
        d.outputName = o->name.empty() ? "wl_output" : o->name;
        d.monitorName = !o->description.empty() ? o->description : (o->make + " " + o->model);
        d.width = o->width;
        d.height = o->height;
        d.refreshHz = o->refreshMhz / 1000.0;
    }
    // wl_output names match the kernel's connector names on the common compositors (KWin, Mutter, wlroots).
    const std::string conn = o ? o->name : std::string();
    if (!edidRead_ || conn != edidConnector_) {
        edid_ = readSysfsEdid(conn);
        edidConnector_ = conn;
        edidRead_ = true;
    }
    d.edid = edid_;
    d.extra = "Wayland reports the nominal mode only; no protocol reports VRR state.";
    if (contentType_) d.extra += " content-type=game set.";
    return d;
}

const char* WaylandWindow::surfaceExtension() const { return VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME; }

VkResult WaylandWindow::createSurface(VkInstance instance, VkSurfaceKHR* surface) {
    VkWaylandSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_WAYLAND_SURFACE_CREATE_INFO_KHR;
    ci.display = display_;
    ci.surface = surface_;
    return vkCreateWaylandSurfaceKHR(instance, &ci, nullptr, surface);
}

}  // namespace vrr
