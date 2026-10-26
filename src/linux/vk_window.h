// A window that can host a Vulkan WSI surface (Wayland or X11).
#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>

#include "../common/backend.h"

namespace vrr {

class VkWindow : public Platform {
public:
    virtual const char* surfaceExtension() const = 0;
    virtual VkResult createSurface(VkInstance instance, VkSurfaceKHR* surface) = 0;
    // Current client-area size in pixels.
    virtual void extent(uint32_t& w, uint32_t& h) const = 0;
};

// Maps a Linux evdev key code (linux/input-event-codes.h) to an app key.
Key keyFromEvdev(uint32_t code);

}  // namespace vrr
