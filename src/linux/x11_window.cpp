#include "x11_window.h"

#include <linux/input-event-codes.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#ifdef VRRDIAG_HAVE_XCB_RANDR
#include <xcb/randr.h>
#endif

#include <vulkan/vulkan_xcb.h>

#include "sysfs_edid.h"

namespace vrr {

X11Window::~X11Window() {
    if (conn_) {
        if (window_) xcb_destroy_window(conn_, window_);
        xcb_disconnect(conn_);
    }
}

xcb_atom_t X11Window::atom(const char* name) {
    xcb_intern_atom_reply_t* r =
        xcb_intern_atom_reply(conn_, xcb_intern_atom(conn_, 0, static_cast<uint16_t>(std::strlen(name)), name), nullptr);
    xcb_atom_t a = r ? r->atom : 0;
    std::free(r);
    return a;
}

bool X11Window::init(const Config& cfg, std::string& err) {
    int screenIndex = 0;
    conn_ = xcb_connect(nullptr, &screenIndex);
    if (!conn_ || xcb_connection_has_error(conn_)) return err = "cannot connect to X server (DISPLAY)", false;
    xcb_screen_iterator_t it = xcb_setup_roots_iterator(xcb_get_setup(conn_));
    for (int i = 0; i < screenIndex; ++i) xcb_screen_next(&it);
    screen_ = it.data;

    mode_ = cfg.windowMode == WindowMode::Windowed ? WindowMode::Windowed : WindowMode::Borderless;
    width_ = static_cast<uint32_t>(cfg.windowW);
    height_ = static_cast<uint32_t>(cfg.windowH);

    window_ = xcb_generate_id(conn_);
    const uint32_t mask = XCB_CW_BACK_PIXMAP | XCB_CW_EVENT_MASK;
    const uint32_t values[2] = {XCB_BACK_PIXMAP_NONE, XCB_EVENT_MASK_KEY_PRESS | XCB_EVENT_MASK_STRUCTURE_NOTIFY |
                                                          XCB_EVENT_MASK_FOCUS_CHANGE};
    xcb_create_window(conn_, XCB_COPY_FROM_PARENT, window_, screen_->root, 0, 0, static_cast<uint16_t>(width_),
                      static_cast<uint16_t>(height_), 0, XCB_WINDOW_CLASS_INPUT_OUTPUT, screen_->root_visual, mask,
                      values);

    const char* title = "Bzzrt - VRR flicker check";
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, XCB_ATOM_WM_NAME, XCB_ATOM_STRING, 8,
                        static_cast<uint32_t>(std::strlen(title)), title);
    wmProtocols_ = atom("WM_PROTOCOLS");
    wmDelete_ = atom("WM_DELETE_WINDOW");
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, wmProtocols_, XCB_ATOM_ATOM, 32, 1, &wmDelete_);

    // Ask the compositor to unredirect us and tell Mesa/AMD's DDX the window wants VRR.
    const uint32_t one = 1;
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, atom("_NET_WM_BYPASS_COMPOSITOR"), XCB_ATOM_CARDINAL,
                        32, 1, &one);
    xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, atom("_VARIABLE_REFRESH"), XCB_ATOM_CARDINAL, 32, 1,
                        &one);

    netWmState_ = atom("_NET_WM_STATE");
    netWmStateFullscreen_ = atom("_NET_WM_STATE_FULLSCREEN");
    if (mode_ == WindowMode::Borderless)
        xcb_change_property(conn_, XCB_PROP_MODE_REPLACE, window_, netWmState_, XCB_ATOM_ATOM, 32, 1,
                            &netWmStateFullscreen_);

    // Invisible cursor.
    xcb_pixmap_t pix = xcb_generate_id(conn_);
    xcb_create_pixmap(conn_, 1, pix, window_, 1, 1);
    xcb_cursor_t cursor = xcb_generate_id(conn_);
    xcb_create_cursor(conn_, cursor, pix, pix, 0, 0, 0, 0, 0, 0, 0, 0);
    xcb_change_window_attributes(conn_, window_, XCB_CW_CURSOR, &cursor);
    xcb_free_cursor(conn_, cursor);
    xcb_free_pixmap(conn_, pix);

    xcb_map_window(conn_, window_);
    xcb_flush(conn_);
    return true;
}

void X11Window::pollEvents(std::vector<KeyEvent>& keys) {
    while (xcb_generic_event_t* ev = xcb_poll_for_event(conn_)) {
        switch (ev->response_type & 0x7f) {
            case XCB_KEY_PRESS: {
                auto* kp = reinterpret_cast<xcb_key_press_event_t*>(ev);
                const Key k = keyFromEvdev(kp->detail - 8u);
                if (k != Key::None) keys.push_back({k, (kp->state & XCB_MOD_MASK_SHIFT) != 0});
                break;
            }
            case XCB_CONFIGURE_NOTIFY: {
                auto* cn = reinterpret_cast<xcb_configure_notify_event_t*>(ev);
                width_ = cn->width;
                height_ = cn->height;
                displayDirty_ = true;
                break;
            }
            case XCB_CLIENT_MESSAGE: {
                auto* cm = reinterpret_cast<xcb_client_message_event_t*>(ev);
                if (cm->type == wmProtocols_ && cm->data.data32[0] == wmDelete_) closed_ = true;
                break;
            }
            default: break;
        }
        std::free(ev);
    }
    if (xcb_connection_has_error(conn_)) closed_ = true;
}

void X11Window::setWindowMode(WindowMode m) {
    if (m == WindowMode::Exclusive) m = WindowMode::Borderless;
    if (m == mode_) return;
    mode_ = m;
    xcb_client_message_event_t ev{};
    ev.response_type = XCB_CLIENT_MESSAGE;
    ev.format = 32;
    ev.window = window_;
    ev.type = netWmState_;
    ev.data.data32[0] = m == WindowMode::Borderless ? 1 : 0;  // _NET_WM_STATE_ADD / REMOVE
    ev.data.data32[1] = netWmStateFullscreen_;
    ev.data.data32[3] = 1;
    xcb_send_event(conn_, 0, screen_->root,
                   XCB_EVENT_MASK_SUBSTRUCTURE_REDIRECT | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY,
                   reinterpret_cast<const char*>(&ev));
    xcb_flush(conn_);
}

void X11Window::queryDisplay() {
    cached_ = DisplayInfo{};
    cached_.outputName = "X11 screen";
    cached_.width = screen_->width_in_pixels;
    cached_.height = screen_->height_in_pixels;
    cached_.extra = "X11: VRR depends on the DDX/driver (AMD: VariableRefresh option; NVIDIA: G-SYNC setting). "
                    "_VARIABLE_REFRESH=1 is set on this window.";
#ifdef VRRDIAG_HAVE_XCB_RANDR
    xcb_translate_coordinates_reply_t* tr =
        xcb_translate_coordinates_reply(conn_, xcb_translate_coordinates(conn_, window_, screen_->root, 0, 0), nullptr);
    const int cx = (tr ? tr->dst_x : 0) + static_cast<int>(width_) / 2;
    const int cy = (tr ? tr->dst_y : 0) + static_cast<int>(height_) / 2;
    std::free(tr);

    xcb_randr_get_screen_resources_current_reply_t* res = xcb_randr_get_screen_resources_current_reply(
        conn_, xcb_randr_get_screen_resources_current(conn_, screen_->root), nullptr);
    if (!res) return;
    const xcb_randr_crtc_t* crtcs = xcb_randr_get_screen_resources_current_crtcs(res);
    const int ncrtc = xcb_randr_get_screen_resources_current_crtcs_length(res);
    const xcb_randr_mode_info_t* modes = xcb_randr_get_screen_resources_current_modes(res);
    const int nmodes = xcb_randr_get_screen_resources_current_modes_length(res);
    for (int i = 0; i < ncrtc; ++i) {
        xcb_randr_get_crtc_info_reply_t* ci =
            xcb_randr_get_crtc_info_reply(conn_, xcb_randr_get_crtc_info(conn_, crtcs[i], res->config_timestamp), nullptr);
        if (!ci) continue;
        const bool inside = ci->mode != 0 && cx >= ci->x && cy >= ci->y && cx < ci->x + ci->width &&
                            cy < ci->y + ci->height;
        if (inside) {
            cached_.width = ci->width;
            cached_.height = ci->height;
            for (int m = 0; m < nmodes; ++m) {
                if (modes[m].id != ci->mode) continue;
                double vtotal = modes[m].vtotal;
                if (modes[m].mode_flags & XCB_RANDR_MODE_FLAG_DOUBLE_SCAN) vtotal *= 2;
                if (modes[m].mode_flags & XCB_RANDR_MODE_FLAG_INTERLACE) vtotal /= 2;
                if (modes[m].htotal && vtotal > 0)
                    cached_.refreshHz = static_cast<double>(modes[m].dot_clock) / (modes[m].htotal * vtotal);
            }
            if (xcb_randr_get_crtc_info_outputs_length(ci) > 0) {
                xcb_randr_get_output_info_reply_t* oi = xcb_randr_get_output_info_reply(
                    conn_,
                    xcb_randr_get_output_info(conn_, xcb_randr_get_crtc_info_outputs(ci)[0], res->config_timestamp),
                    nullptr);
                if (oi) {
                    cached_.outputName.assign(reinterpret_cast<const char*>(xcb_randr_get_output_info_name(oi)),
                                              xcb_randr_get_output_info_name_length(oi));
                    std::free(oi);
                }
                // The EDID output property; DDX output names do not always match the kernel's (amdgpu uses
                // "DisplayPort-0"), so sysfs is only the fallback.
                const xcb_randr_output_t out = xcb_randr_get_crtc_info_outputs(ci)[0];
                xcb_randr_get_output_property_reply_t* pr = xcb_randr_get_output_property_reply(
                    conn_, xcb_randr_get_output_property(conn_, out, atom("EDID"), XCB_ATOM_ANY, 0, 256, 0, 0), nullptr);
                if (pr) {
                    const uint8_t* data = xcb_randr_get_output_property_data(pr);
                    const int len = xcb_randr_get_output_property_data_length(pr);
                    if (pr->format == 8 && len >= 128) cached_.edid.assign(data, data + len);
                    std::free(pr);
                }
            }
        }
        std::free(ci);
        if (inside) break;
    }
    std::free(res);
#endif
    if (cached_.edid.empty()) cached_.edid = readSysfsEdid(cached_.outputName == "X11 screen" ? "" : cached_.outputName);
}

DisplayInfo X11Window::displayInfo() {
    if (displayDirty_) {
        queryDisplay();
        displayDirty_ = false;
    }
    return cached_;
}

const char* X11Window::surfaceExtension() const { return VK_KHR_XCB_SURFACE_EXTENSION_NAME; }

VkResult X11Window::createSurface(VkInstance instance, VkSurfaceKHR* surface) {
    VkXcbSurfaceCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_XCB_SURFACE_CREATE_INFO_KHR;
    ci.connection = conn_;
    ci.window = window_;
    return vkCreateXcbSurfaceKHR(instance, &ci, nullptr, surface);
}

}  // namespace vrr
