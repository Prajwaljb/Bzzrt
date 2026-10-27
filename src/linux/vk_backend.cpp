#include "vk_backend.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>

#include "../common/clock.h"
#include "../common/overlay.h"
#include "../common/patterns.h"
#include "pattern_frag.h"
#include "pattern_vert.h"

namespace vrr {

namespace {

const char* vkResultName(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_SUBOPTIMAL_KHR: return "VK_SUBOPTIMAL_KHR";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_ERROR_OUT_OF_DATE_KHR: return "VK_ERROR_OUT_OF_DATE_KHR";
        case VK_ERROR_SURFACE_LOST_KHR: return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT: return "VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT";
        default: return "VK_ERROR_OTHER";
    }
}

const char* presentModeId(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo_relaxed";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
        default: return "other";
    }
}

const char* presentModeName(VkPresentModeKHR m) {
    switch (m) {
        case VK_PRESENT_MODE_FIFO_KHR: return "FIFO (vsync/no tearing; VRR-capable if the compositor enables VRR)";
        case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "FIFO_RELAXED (vsync; late frames may tear)";
        case VK_PRESENT_MODE_MAILBOX_KHR: return "MAILBOX (no tearing; newest frame wins; may drop frames)";
        case VK_PRESENT_MODE_IMMEDIATE_KHR: return "IMMEDIATE (no vsync; tearing allowed)";
        default: return "other";
    }
}

const char* formatName(VkFormat f) {
    switch (f) {
        case VK_FORMAT_B8G8R8A8_UNORM: return "VK_FORMAT_B8G8R8A8_UNORM";
        case VK_FORMAT_R8G8B8A8_UNORM: return "VK_FORMAT_R8G8B8A8_UNORM";
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return "VK_FORMAT_A8B8G8R8_UNORM_PACK32";
        case VK_FORMAT_B8G8R8A8_SRGB: return "VK_FORMAT_B8G8R8A8_SRGB";
        case VK_FORMAT_R8G8B8A8_SRGB: return "VK_FORMAT_R8G8B8A8_SRGB";
        case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "VK_FORMAT_A2B10G10R10_UNORM_PACK32";
        case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "VK_FORMAT_A2R10G10B10_UNORM_PACK32";
        case VK_FORMAT_R16G16B16A16_SFLOAT: return "VK_FORMAT_R16G16B16A16_SFLOAT";
        default: return "VK_FORMAT_other";
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                             VkDebugUtilsMessageTypeFlagsEXT,
                                             const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr, "[vulkan] %s\n", data->pMessage);
    return VK_FALSE;
}

bool hasExtension(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts)
        if (!std::strcmp(e.extensionName, name)) return true;
    return false;
}

}  // namespace

VulkanBackend::~VulkanBackend() {
    pwStop_ = true;
    pwCv_.notify_all();
    if (pwThread_.joinable()) pwThread_.join();
    if (device_) {
        vkDeviceWaitIdle(device_);
        destroySwapchainResources();
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        for (auto& f : frames_) {
            if (f.stagingMem) vkFreeMemory(device_, f.stagingMem, nullptr);
            if (f.staging) vkDestroyBuffer(device_, f.staging, nullptr);
            if (f.fence) vkDestroyFence(device_, f.fence, nullptr);
            if (f.acquired) vkDestroySemaphore(device_, f.acquired, nullptr);
        }
        if (cmdPool_) vkDestroyCommandPool(device_, cmdPool_, nullptr);
        if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
        if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
        if (pipeLayout_) vkDestroyPipelineLayout(device_, pipeLayout_, nullptr);
        if (dpool_) vkDestroyDescriptorPool(device_, dpool_, nullptr);
        if (dsl_) vkDestroyDescriptorSetLayout(device_, dsl_, nullptr);
        if (sampler_) vkDestroySampler(device_, sampler_, nullptr);
        if (vs_) vkDestroyShaderModule(device_, vs_, nullptr);
        if (fs_) vkDestroyShaderModule(device_, fs_, nullptr);
        if (overlayView_) vkDestroyImageView(device_, overlayView_, nullptr);
        if (overlayImg_) vkDestroyImage(device_, overlayImg_, nullptr);
        if (overlayMem_) vkFreeMemory(device_, overlayMem_, nullptr);
        if (userView_) vkDestroyImageView(device_, userView_, nullptr);
        if (userImg_) vkDestroyImage(device_, userImg_, nullptr);
        if (userMem_) vkFreeMemory(device_, userMem_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (messenger_) {
        auto fn = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkDestroyDebugUtilsMessengerEXT"));
        if (fn) fn(instance_, messenger_, nullptr);
    }
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

bool VulkanBackend::init(const Config& cfg, const ImageData& image, std::string& err) {
    want10_ = cfg.tenBit;
    if (!createInstance(cfg, err)) return false;
    if (win_.createSurface(instance_, &surface_) != VK_SUCCESS) return err = "vkCreate*SurfaceKHR failed", false;
    if (!pickDevice(cfg, err)) return false;
    if (!createDevice(err)) return false;

    uint32_t n = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, nullptr);
    std::vector<VkPresentModeKHR> avail(n);
    vkGetPhysicalDeviceSurfacePresentModesKHR(phys_, surface_, &n, avail.data());
    for (VkPresentModeKHR m : {VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_FIFO_RELAXED_KHR, VK_PRESENT_MODE_MAILBOX_KHR,
                               VK_PRESENT_MODE_IMMEDIATE_KHR})
        if (std::find(avail.begin(), avail.end(), m) != avail.end()) modes_.push_back(m);
    modeIdx_ = 0;  // FIFO is always supported

    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, nullptr);
    surfaceFormats_.resize(n);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys_, surface_, &n, surfaceFormats_.data());
    for (const auto& f : surfaceFormats_)
        if ((f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 || f.format == VK_FORMAT_A2R10G10B10_UNORM_PACK32) &&
            f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            has10bit_ = true;

    if (!createPipelineObjects(err)) return false;
    if (!createTextures(image, err)) return false;

    std::printf("Vulkan GPUs:\n");
    for (const auto& g : gpuList_) std::printf("  %s\n", g.c_str());
    std::printf("Present modes:");
    for (auto m : modes_) std::printf(" %s", presentModeId(m));
    std::printf("\nVK_KHR_present_id: %s  VK_KHR_present_wait: %s  VK_GOOGLE_display_timing: %s\n",
                hasPresentId_ ? "yes" : "no", hasPresentWait_ ? "yes" : "no", hasDisplayTiming_ ? "yes" : "no");

    if (hasPresentWait_) pwThread_ = std::thread([this] { presentWaitLoop(); });
    return true;
}

bool VulkanBackend::createInstance(const Config& cfg, std::string& err) {
    uint32_t n = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateInstanceExtensionProperties(nullptr, &n, exts.data());

    std::vector<const char*> enable = {VK_KHR_SURFACE_EXTENSION_NAME, win_.surfaceExtension()};
    for (const char* e : enable)
        if (!hasExtension(exts, e)) return err = std::string("missing instance extension ") + e, false;
    std::vector<const char*> layers;
    const bool debug = cfg.debug && hasExtension(exts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (debug) {
        enable.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
        vkEnumerateInstanceLayerProperties(&n, nullptr);
        std::vector<VkLayerProperties> lp(n);
        vkEnumerateInstanceLayerProperties(&n, lp.data());
        for (const auto& l : lp)
            if (!std::strcmp(l.layerName, "VK_LAYER_KHRONOS_validation")) layers.push_back("VK_LAYER_KHRONOS_validation");
    }

    VkApplicationInfo app{};
    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.pApplicationName = "vrrdiag";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    ci.ppEnabledExtensionNames = enable.data();
    ci.enabledLayerCount = static_cast<uint32_t>(layers.size());
    ci.ppEnabledLayerNames = layers.data();
    VkResult r = vkCreateInstance(&ci, nullptr, &instance_);
    if (r != VK_SUCCESS) return err = std::string("vkCreateInstance failed: ") + vkResultName(r), false;

    if (debug) {
        auto fn = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(instance_, "vkCreateDebugUtilsMessengerEXT"));
        VkDebugUtilsMessengerCreateInfoEXT mi{};
        mi.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
        mi.messageSeverity =
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
        mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
        mi.pfnUserCallback = debugCallback;
        if (fn) fn(instance_, &mi, nullptr, &messenger_);
    }
    return true;
}

bool VulkanBackend::pickDevice(const Config& cfg, std::string& err) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(instance_, &n, nullptr);
    std::vector<VkPhysicalDevice> devs(n);
    vkEnumeratePhysicalDevices(instance_, &n, devs.data());
    if (devs.empty()) return err = "no Vulkan devices", false;

    int chosen = -1;
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &qn, qf.data());
        int family = -1;
        for (uint32_t q = 0; q < qn; ++q) {
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(devs[i], q, surface_, &present);
            if ((qf[q].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                family = static_cast<int>(q);
                break;
            }
        }
        char line[512];
        std::snprintf(line, sizeof line, "[%u] %s%s", i, p.deviceName, family < 0 ? " (cannot present to this surface)" : "");
        gpuList_.push_back(line);
        const bool wanted = cfg.gpuIndex >= 0 ? static_cast<int>(i) == cfg.gpuIndex : chosen < 0;
        if (wanted && family >= 0) {
            chosen = static_cast<int>(i);
            queueFamily_ = static_cast<uint32_t>(family);
        }
    }
    if (chosen < 0)
        return err = cfg.gpuIndex >= 0 ? "requested --gpu cannot present to this surface"
                                       : "no GPU can present to this surface",
               false;
    phys_ = devs[chosen];

    VkPhysicalDeviceProperties p;
    vkGetPhysicalDeviceProperties(phys_, &p);
    gpuName_ = p.deviceName;
    char ver[64];
    std::snprintf(ver, sizeof ver, "%u.%u.%u", VK_API_VERSION_MAJOR(p.apiVersion), VK_API_VERSION_MINOR(p.apiVersion),
                  VK_API_VERSION_PATCH(p.apiVersion));
    apiVersion_ = ver;
    if (p.apiVersion >= VK_API_VERSION_1_2) {
        VkPhysicalDeviceDriverProperties drv{};
        drv.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES;
        VkPhysicalDeviceProperties2 p2{};
        p2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        p2.pNext = &drv;
        vkGetPhysicalDeviceProperties2(phys_, &p2);
        driverInfo_ = std::string(drv.driverName) + " " + drv.driverInfo;
    }
    vkGetPhysicalDeviceMemoryProperties(phys_, &memProps_);
    return true;
}

bool VulkanBackend::createDevice(std::string& err) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> exts(n);
    vkEnumerateDeviceExtensionProperties(phys_, nullptr, &n, exts.data());
    if (!hasExtension(exts, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return err = "VK_KHR_swapchain missing", false;

    VkPhysicalDevicePresentWaitFeaturesKHR pwf{};
    pwf.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
    VkPhysicalDevicePresentIdFeaturesKHR pif{};
    pif.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
    pif.pNext = &pwf;
    VkPhysicalDeviceFeatures2 f2{};
    f2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    f2.pNext = &pif;
    vkGetPhysicalDeviceFeatures2(phys_, &f2);

    hasPresentId_ = hasExtension(exts, VK_KHR_PRESENT_ID_EXTENSION_NAME) && pif.presentId;
    hasPresentWait_ = hasPresentId_ && hasExtension(exts, VK_KHR_PRESENT_WAIT_EXTENSION_NAME) && pwf.presentWait;
    hasDisplayTiming_ = hasExtension(exts, VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);

    std::vector<const char*> enable = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    void* chain = nullptr;
    VkPhysicalDevicePresentIdFeaturesKHR enId{};
    VkPhysicalDevicePresentWaitFeaturesKHR enWait{};
    if (hasPresentId_) {
        enable.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
        enId.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR;
        enId.presentId = VK_TRUE;
        enId.pNext = chain;
        chain = &enId;
    }
    if (hasPresentWait_) {
        enable.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
        enWait.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR;
        enWait.presentWait = VK_TRUE;
        enWait.pNext = chain;
        chain = &enWait;
    }
    if (hasDisplayTiming_) enable.push_back(VK_GOOGLE_DISPLAY_TIMING_EXTENSION_NAME);

    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = queueFamily_;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.pNext = chain;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = static_cast<uint32_t>(enable.size());
    dci.ppEnabledExtensionNames = enable.data();
    VkResult r = vkCreateDevice(phys_, &dci, nullptr, &device_);
    if (r != VK_SUCCESS) return err = std::string("vkCreateDevice failed: ") + vkResultName(r), false;
    vkGetDeviceQueue(device_, queueFamily_, 0, &queue_);

    if (hasPresentWait_)
        pfnWaitForPresent_ = reinterpret_cast<PFN_vkWaitForPresentKHR>(vkGetDeviceProcAddr(device_, "vkWaitForPresentKHR"));
    if (hasDisplayTiming_) {
        pfnPastTiming_ = reinterpret_cast<PFN_vkGetPastPresentationTimingGOOGLE>(
            vkGetDeviceProcAddr(device_, "vkGetPastPresentationTimingGOOGLE"));
        pfnRefreshCycle_ = reinterpret_cast<PFN_vkGetRefreshCycleDurationGOOGLE>(
            vkGetDeviceProcAddr(device_, "vkGetRefreshCycleDurationGOOGLE"));
    }
    hasPresentWait_ = hasPresentWait_ && pfnWaitForPresent_;
    hasDisplayTiming_ = hasDisplayTiming_ && pfnPastTiming_;
    return true;
}

uint32_t VulkanBackend::findMemoryType(uint32_t bits, VkMemoryPropertyFlags props) const {
    for (uint32_t i = 0; i < memProps_.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (memProps_.memoryTypes[i].propertyFlags & props) == props) return i;
    return UINT32_MAX;
}

bool VulkanBackend::createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props,
                                 VkBuffer& buf, VkDeviceMemory& mem) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device_, buf, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, props);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    return vkBindBufferMemory(device_, buf, mem, 0) == VK_SUCCESS;
}

bool VulkanBackend::createImage(uint32_t w, uint32_t h, VkImage& img, VkDeviceMemory& mem, VkImageView& view) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device_, &ii, nullptr, &img) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device_, img, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || vkAllocateMemory(device_, &ai, nullptr, &mem) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, img, mem, 0);
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_UNORM;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return vkCreateImageView(device_, &vi, nullptr, &view) == VK_SUCCESS;
}

VkCommandBuffer VulkanBackend::beginOneShot() {
    VkCommandBufferAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = cmdPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device_, &ai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    return cmd;
}

void VulkanBackend::endOneShot(VkCommandBuffer cmd) {
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue_, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue_);
    vkFreeCommandBuffers(device_, cmdPool_, 1, &cmd);
}

static void imageBarrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                         VkAccessFlags srcAccess, VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                         VkPipelineStageFlags dstStage) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = srcAccess;
    b.dstAccessMask = dstAccess;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

bool VulkanBackend::createPipelineObjects(std::string& err) {
    VkCommandPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = queueFamily_;
    if (vkCreateCommandPool(device_, &pci, nullptr, &cmdPool_) != VK_SUCCESS) return err = "command pool", false;

    const VkDeviceSize stagingSize = static_cast<VkDeviceSize>(kOverlayW) * kOverlayH * 4;
    for (auto& f : frames_) {
        VkCommandBufferAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        ai.commandPool = cmdPool_;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(device_, &ai, &f.cmd);
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(device_, &fi, nullptr, &f.fence);
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        vkCreateSemaphore(device_, &si, nullptr, &f.acquired);
        if (!createBuffer(stagingSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, f.staging,
                          f.stagingMem))
            return err = "staging buffer allocation failed", false;
        vkMapMemory(device_, f.stagingMem, 0, stagingSize, 0, &f.stagingPtr);
    }

    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = sizeof(kPatternVertSpv);
    smi.pCode = kPatternVertSpv;
    if (vkCreateShaderModule(device_, &smi, nullptr, &vs_) != VK_SUCCESS) return err = "vertex shader", false;
    smi.codeSize = sizeof(kPatternFragSpv);
    smi.pCode = kPatternFragSpv;
    if (vkCreateShaderModule(device_, &smi, nullptr, &fs_) != VK_SUCCESS) return err = "fragment shader", false;

    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.maxLod = 0.0f;
    vkCreateSampler(device_, &sci, nullptr, &sampler_);

    VkDescriptorSetLayoutBinding binds[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        binds[i].binding = i;
        binds[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binds[i].descriptorCount = 1;
        binds[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo dli{};
    dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dli.bindingCount = 2;
    dli.pBindings = binds;
    vkCreateDescriptorSetLayout(device_, &dli, nullptr, &dsl_);

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ShaderParams)};
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &dsl_;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(device_, &pli, nullptr, &pipeLayout_);

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 1;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    vkCreateDescriptorPool(device_, &dpi, nullptr, &dpool_);
    VkDescriptorSetAllocateInfo dai{};
    dai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dai.descriptorPool = dpool_;
    dai.descriptorSetCount = 1;
    dai.pSetLayouts = &dsl_;
    if (vkAllocateDescriptorSets(device_, &dai, &dset_) != VK_SUCCESS) return err = "descriptor set", false;
    return true;
}

bool VulkanBackend::createRenderPassAndPipeline(VkFormat format) {
    if (renderPass_ && renderPassFormat_ == format) return true;
    vkDeviceWaitIdle(device_);
    if (pipeline_) vkDestroyPipeline(device_, pipeline_, nullptr);
    if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
    pipeline_ = VK_NULL_HANDLE;
    renderPass_ = VK_NULL_HANDLE;

    VkAttachmentDescription att{};
    att.format = format;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // every pixel is written
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.dstSubpass = 0;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpi{};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1;
    rpi.pAttachments = &att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &sub;
    rpi.dependencyCount = 1;
    rpi.pDependencies = &dep;
    if (vkCreateRenderPass(device_, &rpi, nullptr, &renderPass_) != VK_SUCCESS) return false;
    renderPassFormat_ = format;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs_;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs_;
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{};
    vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                         VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{};
    ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo gpi{};
    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vp;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pColorBlendState = &cb;
    gpi.pDynamicState = &ds;
    gpi.layout = pipeLayout_;
    gpi.renderPass = renderPass_;
    return vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline_) == VK_SUCCESS;
}

bool VulkanBackend::createTextures(const ImageData& image, std::string& err) {
    if (!createImage(kOverlayW, kOverlayH, overlayImg_, overlayMem_, overlayView_))
        return err = "overlay texture allocation failed", false;
    hasImage_ = image.valid();
    const uint32_t iw = hasImage_ ? static_cast<uint32_t>(image.width) : 1;
    const uint32_t ih = hasImage_ ? static_cast<uint32_t>(image.height) : 1;
    imageW_ = static_cast<float>(iw);
    imageH_ = static_cast<float>(ih);
    if (!createImage(iw, ih, userImg_, userMem_, userView_)) return err = "image texture allocation failed", false;

    VkBuffer staging;
    VkDeviceMemory stagingMem;
    const VkDeviceSize size = static_cast<VkDeviceSize>(iw) * ih * 4;
    if (!createBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, staging, stagingMem))
        return err = "upload buffer allocation failed", false;
    void* ptr = nullptr;
    vkMapMemory(device_, stagingMem, 0, size, 0, &ptr);
    const uint32_t gray = 0xFF333333u;
    std::memcpy(ptr, hasImage_ ? static_cast<const void*>(image.rgba.data()) : &gray, size);
    vkUnmapMemory(device_, stagingMem);

    VkCommandBuffer cmd = beginOneShot();
    imageBarrier(cmd, overlayImg_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkClearColorValue zero{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, overlayImg_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    imageBarrier(cmd, overlayImg_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    imageBarrier(cmd, userImg_, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {iw, ih, 1};
    vkCmdCopyBufferToImage(cmd, staging, userImg_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    imageBarrier(cmd, userImg_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    endOneShot(cmd);
    vkDestroyBuffer(device_, staging, nullptr);
    vkFreeMemory(device_, stagingMem, nullptr);

    VkDescriptorImageInfo infos[2] = {
        {sampler_, overlayView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler_, userView_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
    };
    VkWriteDescriptorSet w[2]{};
    for (uint32_t i = 0; i < 2; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstSet = dset_;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[i].pImageInfo = &infos[i];
    }
    vkUpdateDescriptorSets(device_, 2, w, 0, nullptr);
    return true;
}

VkSurfaceFormatKHR VulkanBackend::chooseFormat(bool tenBit, bool& srgb, bool& is10) const {
    auto find = [&](VkFormat f) -> const VkSurfaceFormatKHR* {
        for (const auto& s : surfaceFormats_)
            if (s.format == f && s.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) return &s;
        return nullptr;
    };
    srgb = false;
    is10 = false;
    if (tenBit) {
        for (VkFormat f : {VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2R10G10B10_UNORM_PACK32})
            if (auto* s = find(f)) {
                is10 = true;
                return *s;
            }
    }
    for (VkFormat f : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_A8B8G8R8_UNORM_PACK32})
        if (auto* s = find(f)) return *s;
    for (VkFormat f : {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB})
        if (auto* s = find(f)) {
            srgb = true;
            return *s;
        }
    return surfaceFormats_.front();
}

void VulkanBackend::destroySwapchainResources() {
    for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    for (auto v : views_) vkDestroyImageView(device_, v, nullptr);
    for (auto s : renderDone_) vkDestroySemaphore(device_, s, nullptr);
    framebuffers_.clear();
    views_.clear();
    renderDone_.clear();
    images_.clear();
}

bool VulkanBackend::recreateSwapchain() {
    uint32_t ww = 0, wh = 0;
    win_.extent(ww, wh);
    VkSurfaceCapabilitiesKHR caps;
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys_, surface_, &caps) != VK_SUCCESS) return false;
    VkExtent2D ext = caps.currentExtent;
    if (ext.width == UINT32_MAX) {
        ext.width = std::clamp(ww, caps.minImageExtent.width, caps.maxImageExtent.width);
        ext.height = std::clamp(wh, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (ext.width == 0 || ext.height == 0) return false;

    vkDeviceWaitIdle(device_);
    format_ = chooseFormat(want10_, srgbTarget_, is10_);
    if (!createRenderPassAndPipeline(format_.format)) return false;

    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount) count = caps.maxImageCount;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha)) {
        for (VkCompositeAlphaFlagBitsKHR a : {VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
                                              VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR})
            if (caps.supportedCompositeAlpha & a) {
                alpha = a;
                break;
            }
    }

    VkSwapchainCreateInfoKHR ci{};
    ci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    ci.surface = surface_;
    ci.minImageCount = count;
    ci.imageFormat = format_.format;
    ci.imageColorSpace = format_.colorSpace;
    ci.imageExtent = ext;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = alpha;
    ci.presentMode = modes_[modeIdx_];
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = swapchain_;

    VkSwapchainKHR fresh = VK_NULL_HANDLE;
    VkResult r = vkCreateSwapchainKHR(device_, &ci, nullptr, &fresh);
    {
        std::lock_guard<std::mutex> lk(swapMutex_);
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = r == VK_SUCCESS ? fresh : VK_NULL_HANDLE;
        ++swapGen_;
    }
    {
        std::lock_guard<std::mutex> lk(pwMutex_);
        pwPending_.clear();
    }
    destroySwapchainResources();
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "vkCreateSwapchainKHR failed: %s\n", vkResultName(r));
        return false;
    }
    extent_ = ext;

    uint32_t n = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, nullptr);
    images_.resize(n);
    vkGetSwapchainImagesKHR(device_, swapchain_, &n, images_.data());
    for (VkImage img : images_) {
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = img;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = format_.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView view;
        vkCreateImageView(device_, &vi, nullptr, &view);
        views_.push_back(view);
        VkFramebufferCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fi.renderPass = renderPass_;
        fi.attachmentCount = 1;
        fi.pAttachments = &view;
        fi.width = ext.width;
        fi.height = ext.height;
        fi.layers = 1;
        VkFramebuffer fb;
        vkCreateFramebuffer(device_, &fi, nullptr, &fb);
        framebuffers_.push_back(fb);
        VkSemaphoreCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        VkSemaphore s;
        vkCreateSemaphore(device_, &si, nullptr, &s);
        renderDone_.push_back(s);
    }

    if (hasDisplayTiming_ && pfnRefreshCycle_) {
        VkRefreshCycleDurationGOOGLE rc{};
        if (pfnRefreshCycle_(device_, swapchain_, &rc) == VK_SUCCESS) refreshCycleNs_ = rc.refreshDuration;
    }
    needRecreate_ = false;
    return true;
}

RenderResult VulkanBackend::renderFrame(const FrameParams& p) {
    uint32_t ww = 0, wh = 0;
    win_.extent(ww, wh);
    if (ww == 0 || wh == 0) return RenderResult::Skipped;
    if (needRecreate_ || !swapchain_ || ww != extent_.width || wh != extent_.height) {
        if (!recreateSwapchain()) return RenderResult::Skipped;
    }

    Frame& f = frames_[slot_];
    vkWaitForFences(device_, 1, &f.fence, VK_TRUE, UINT64_MAX);
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, 1000000000ull, f.acquired, VK_NULL_HANDLE, &imageIndex_);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        needRecreate_ = true;
        return RenderResult::Skipped;
    }
    if (r == VK_TIMEOUT || r == VK_NOT_READY) return RenderResult::Skipped;
    if (r == VK_ERROR_SURFACE_LOST_KHR || r == VK_ERROR_DEVICE_LOST) {
        std::fprintf(stderr, "vkAcquireNextImageKHR: %s\n", vkResultName(r));
        return RenderResult::Fatal;
    }
    if (r == VK_SUBOPTIMAL_KHR) needRecreate_ = true;
    vkResetFences(device_, 1, &f.fence);

    VkCommandBuffer cmd = f.cmd;
    vkResetCommandBuffer(cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    if (p.overlayPixels && p.overlayVersion != overlayVersion_) {
        std::memcpy(f.stagingPtr, p.overlayPixels, static_cast<size_t>(kOverlayW) * kOverlayH * 4);
        imageBarrier(cmd, overlayImg_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                     VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {static_cast<uint32_t>(kOverlayW), static_cast<uint32_t>(kOverlayH), 1};
        vkCmdCopyBufferToImage(cmd, f.staging, overlayImg_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        imageBarrier(cmd, overlayImg_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                     VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        overlayVersion_ = p.overlayVersion;
    }

    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = renderPass_;
    rpb.framebuffer = framebuffers_[imageIndex_];
    rpb.renderArea.extent = extent_;
    vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    VkViewport vp{0, 0, static_cast<float>(extent_.width), static_cast<float>(extent_.height), 0, 1};
    VkRect2D sc{{0, 0}, extent_};
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeLayout_, 0, 1, &dset_, 0, nullptr);

    ShaderParams sp{};
    sp.resolution[0] = static_cast<float>(extent_.width);
    sp.resolution[1] = static_cast<float>(extent_.height);
    sp.overlayPos[0] = static_cast<float>(p.overlayX);
    sp.overlayPos[1] = static_cast<float>(p.overlayY);
    sp.overlaySize[0] = static_cast<float>(p.overlayW);
    sp.overlaySize[1] = static_cast<float>(p.overlayH);
    sp.moveX = static_cast<float>(moveBarX(p.patternTime, static_cast<int>(extent_.width)));
    sp.pattern = static_cast<uint32_t>(p.pattern);
    sp.flags = (p.overlayVisible ? kFlagOverlay : 0u) | (p.frameCounter ? kFlagCounter : 0u) |
               (hasImage_ ? kFlagImage : 0u) | (srgbTarget_ ? kFlagSrgbTarget : 0u);
    sp.frame = static_cast<uint32_t>(p.frameIndex);
    sp.imageSize[0] = imageW_;
    sp.imageSize[1] = imageH_;
    vkCmdPushConstants(cmd, pipeLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof sp, &sp);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &f.acquired;
    si.pWaitDstStageMask = &waitStage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &renderDone_[imageIndex_];
    r = vkQueueSubmit(queue_, 1, &si, f.fence);
    if (r != VK_SUCCESS) {
        std::fprintf(stderr, "vkQueueSubmit: %s\n", vkResultName(r));
        return RenderResult::Fatal;
    }
    return RenderResult::Rendered;
}

PresentOutcome VulkanBackend::present(uint64_t frameIndex) {
    const uint64_t presentId = ++presentSeq_;
    seqMap_[presentId % kSeqMap] = {presentId, frameIndex};
    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &renderDone_[imageIndex_];
    pi.swapchainCount = 1;
    pi.pSwapchains = &swapchain_;
    pi.pImageIndices = &imageIndex_;

    VkPresentIdKHR idInfo{};
    VkPresentTimeGOOGLE time{};
    VkPresentTimesInfoGOOGLE timesInfo{};
    const void* chain = nullptr;
    if (hasPresentId_) {
        idInfo.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
        idInfo.swapchainCount = 1;
        idInfo.pPresentIds = &presentId;
        idInfo.pNext = chain;
        chain = &idInfo;
    }
    if (hasDisplayTiming_) {
        time.presentID = static_cast<uint32_t>(presentId);
        time.desiredPresentTime = 0;  // present as soon as possible; pacing is done by the CPU timer
        timesInfo.sType = VK_STRUCTURE_TYPE_PRESENT_TIMES_INFO_GOOGLE;
        timesInfo.swapchainCount = 1;
        timesInfo.pTimes = &time;
        timesInfo.pNext = chain;
        chain = &timesInfo;
    }
    pi.pNext = chain;

    VkResult r;
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(swapMutex_);
        r = vkQueuePresentKHR(queue_, &pi);
        gen = swapGen_;
    }
    slot_ = (slot_ + 1) % kFrames;

    PresentOutcome o;
    o.status = vkResultName(r);
    o.ok = r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR;
    if (o.ok && hasPresentWait_) {
        std::lock_guard<std::mutex> lk(pwMutex_);
        pwPending_.emplace_back(presentId, gen);
        pwCv_.notify_one();
    }
    if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) needRecreate_ = true;
    if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR) o.fatal = true;
    return o;
}

void VulkanBackend::presentWaitLoop() {
    std::unique_lock<std::mutex> lk(pwMutex_);
    while (!pwStop_) {
        pwCv_.wait(lk, [this] { return pwStop_ || !pwPending_.empty(); });
        if (pwStop_) break;
        const auto [id, gen] = pwPending_.front();
        lk.unlock();
        bool done = false, drop = false;
        while (!done && !drop && !pwStop_) {
            {
                std::lock_guard<std::mutex> sl(swapMutex_);
                if (gen != swapGen_ || !swapchain_) {
                    drop = true;
                } else {
                    const VkResult r = pfnWaitForPresent_(device_, swapchain_, id, 0);
                    if (r == VK_SUCCESS) done = true;
                    else if (r != VK_TIMEOUT) drop = true;
                }
            }
            if (!done && !drop) std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        const int64_t t = nowNs();
        lk.lock();
        if (!pwPending_.empty() && pwPending_.front().first == id) pwPending_.pop_front();
        if (done) pwResults_.push_back({id, t});
    }
}

void VulkanBackend::pollTiming(std::vector<DisplayTimingSample>& out) {
    if (hasPresentWait_) {
        std::lock_guard<std::mutex> lk(pwMutex_);
        if (!hasDisplayTiming_)
            for (const auto& r : pwResults_)
                if (const uint64_t f = frameForPresent(r.frameIndex)) out.push_back({f, r.displayNs});
        if (!pwResults_.empty()) lastTimingNs_ = nowNs();
        timingSamples_ += pwResults_.size();
        pwResults_.clear();
    }
    if (hasDisplayTiming_ && swapchain_) {
        std::lock_guard<std::mutex> lk(swapMutex_);
        uint32_t n = 0;
        if (pfnPastTiming_(device_, swapchain_, &n, nullptr) == VK_SUCCESS && n > 0) {
            std::vector<VkPastPresentationTimingGOOGLE> v(n);
            if (pfnPastTiming_(device_, swapchain_, &n, v.data()) == VK_SUCCESS || n > 0) {
                for (uint32_t i = 0; i < n; ++i) {
                    // presentID holds the low 32 bits of the present id.
                    uint64_t id = (presentSeq_ & ~0xFFFFFFFFull) | v[i].presentID;
                    if (id > presentSeq_) id -= 0x100000000ull;
                    if (const uint64_t f = frameForPresent(id))
                        out.push_back({f, static_cast<int64_t>(v[i].actualPresentTime)});
                }
                lastTimingNs_ = nowNs();
                timingSamples_ += n;
            }
        }
    }
}

BackendInfo VulkanBackend::info() {
    BackendInfo b;
    b.api = "Vulkan " + apiVersion_ + " / " + win_.name() + " WSI";
    b.gpu = gpuName_ + (driverInfo_.empty() ? "" : " (" + driverInfo_ + ")");
    b.swapFormat = std::string(formatName(format_.format)) + (is10_ ? " (10-bit" : " (8-bit") +
                   (srgbTarget_ ? " sRGB-encoded" : "") + " SRGB_NONLINEAR SDR)";
    if (want10_ && !is10_) b.swapFormat += " - 10-bit requested but not offered by surface";
    b.presentMode = modes_.empty() ? "?" : presentModeName(modes_[modeIdx_]);
    const bool immediate = std::find(modes_.begin(), modes_.end(), VK_PRESENT_MODE_IMMEDIATE_KHR) != modes_.end();
    b.tearing = immediate ? "IMMEDIATE present mode available" : "IMMEDIATE present mode NOT available";
    if (std::string(win_.name()) == "Wayland")
        b.tearing += " (Wayland: tearing also needs compositor tearing-control)";
    const bool recent = lastTimingNs_ != 0 && nowNs() - lastTimingNs_ < 2000000000LL;
    if (hasDisplayTiming_) b.timingSource = "VK_GOOGLE_display_timing actualPresentTime";
    else if (hasPresentWait_) b.timingSource = "VK_KHR_present_wait completion time (polled, +/-0.2 ms)";
    else b.timingSource = "driver exposes neither VK_GOOGLE_display_timing nor VK_KHR_present_wait";
    b.timingAvailable = (hasDisplayTiming_ || hasPresentWait_) && recent;
    if ((hasDisplayTiming_ || hasPresentWait_) && !recent) b.timingSource += " - no samples yet";
    b.width = static_cast<int>(extent_.width);
    b.height = static_cast<int>(extent_.height);
    char buf[256];
    std::snprintf(buf, sizeof buf, "present_id: %s  present_wait: %s  GOOGLE_display_timing: %s  images: %zu",
                  hasPresentId_ ? "yes" : "no", hasPresentWait_ ? "yes" : "no", hasDisplayTiming_ ? "yes" : "no",
                  images_.size());
    b.notes.push_back(buf);
    if (refreshCycleNs_) {
        std::snprintf(buf, sizeof buf, "vkGetRefreshCycleDurationGOOGLE: %.3f ms", refreshCycleNs_ / 1e6);
        b.notes.push_back(buf);
    }
    if (gpuList_.size() > 1) {
        std::string g = "GPUs:";
        for (const auto& s : gpuList_) g += " " + s;
        b.notes.push_back(g + "  (--gpu N)");
    }
    return b;
}

std::vector<PresentModeInfo> VulkanBackend::presentModes() const {
    std::vector<PresentModeInfo> v;
    for (auto m : modes_) v.push_back({presentModeId(m), presentModeName(m), m != VK_PRESENT_MODE_IMMEDIATE_KHR});
    return v;
}

void VulkanBackend::setPresentMode(int index) {
    if (index < 0 || index >= static_cast<int>(modes_.size()) || index == modeIdx_) return;
    modeIdx_ = index;
    needRecreate_ = true;
}

void VulkanBackend::set10bit(bool on) {
    if (on == want10_) return;
    want10_ = on;
    needRecreate_ = true;
}

}  // namespace vrr
