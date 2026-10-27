// Vulkan WSI backend (Wayland / X11 surfaces).
#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "../common/backend.h"
#include "../common/config.h"
#include "../common/image_load.h"
#include "vk_window.h"

namespace vrr {

class VulkanBackend : public Backend {
public:
    explicit VulkanBackend(VkWindow& window) : win_(window) {}
    ~VulkanBackend() override;
    bool init(const Config& cfg, const ImageData& image, std::string& err);

    RenderResult renderFrame(const FrameParams& p) override;
    PresentOutcome present(uint64_t frameIndex) override;
    void pollTiming(std::vector<DisplayTimingSample>& out) override;
    BackendInfo info() override;
    std::vector<PresentModeInfo> presentModes() const override;
    int presentModeIndex() const override { return modeIdx_; }
    void setPresentMode(int index) override;
    bool supports10bit() const override { return has10bit_; }
    bool is10bit() const override { return want10_; }
    void set10bit(bool on) override;
    bool canRepeatFrames() const override { return true; }

private:
    static constexpr int kFrames = 2;
    struct Frame {
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory stagingMem = VK_NULL_HANDLE;
        void* stagingPtr = nullptr;
    };

    bool createInstance(const Config& cfg, std::string& err);
    bool pickDevice(const Config& cfg, std::string& err);
    bool createDevice(std::string& err);
    bool createPipelineObjects(std::string& err);
    bool createRenderPassAndPipeline(VkFormat format);
    bool createTextures(const ImageData& image, std::string& err);
    bool createBuffer(VkDeviceSize size, VkBufferUsageFlags usage, VkMemoryPropertyFlags props, VkBuffer& buf,
                      VkDeviceMemory& mem);
    bool createImage(uint32_t w, uint32_t h, VkImage& img, VkDeviceMemory& mem, VkImageView& view);
    uint32_t findMemoryType(uint32_t bits, VkMemoryPropertyFlags props) const;
    VkCommandBuffer beginOneShot();
    void endOneShot(VkCommandBuffer cmd);
    VkSurfaceFormatKHR chooseFormat(bool tenBit, bool& srgb, bool& is10) const;
    bool recreateSwapchain();
    void destroySwapchainResources();
    void presentWaitLoop();

    VkWindow& win_;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT messenger_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice phys_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    uint32_t queueFamily_ = 0;
    VkPhysicalDeviceMemoryProperties memProps_{};

    bool hasPresentId_ = false, hasPresentWait_ = false, hasDisplayTiming_ = false;
    PFN_vkWaitForPresentKHR pfnWaitForPresent_ = nullptr;
    PFN_vkGetPastPresentationTimingGOOGLE pfnPastTiming_ = nullptr;
    PFN_vkGetRefreshCycleDurationGOOGLE pfnRefreshCycle_ = nullptr;

    std::string gpuName_, driverInfo_, apiVersion_;
    std::vector<std::string> gpuList_;
    std::vector<VkPresentModeKHR> modes_;
    int modeIdx_ = 0;
    std::vector<VkSurfaceFormatKHR> surfaceFormats_;
    bool has10bit_ = false;
    bool want10_ = false;
    bool is10_ = false;
    bool srgbTarget_ = false;
    VkSurfaceFormatKHR format_{};

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    VkExtent2D extent_{0, 0};
    std::vector<VkImage> images_;
    std::vector<VkImageView> views_;
    std::vector<VkFramebuffer> framebuffers_;
    std::vector<VkSemaphore> renderDone_;
    bool needRecreate_ = true;
    uint64_t refreshCycleNs_ = 0;

    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkFormat renderPassFormat_ = VK_FORMAT_UNDEFINED;
    VkPipeline pipeline_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeLayout_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl_ = VK_NULL_HANDLE;
    VkDescriptorPool dpool_ = VK_NULL_HANDLE;
    VkDescriptorSet dset_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    VkShaderModule vs_ = VK_NULL_HANDLE, fs_ = VK_NULL_HANDLE;
    VkCommandPool cmdPool_ = VK_NULL_HANDLE;

    VkImage overlayImg_ = VK_NULL_HANDLE;
    VkDeviceMemory overlayMem_ = VK_NULL_HANDLE;
    VkImageView overlayView_ = VK_NULL_HANDLE;
    uint64_t overlayVersion_ = ~0ull;
    VkImage userImg_ = VK_NULL_HANDLE;
    VkDeviceMemory userMem_ = VK_NULL_HANDLE;
    VkImageView userView_ = VK_NULL_HANDLE;
    bool hasImage_ = false;
    float imageW_ = 1, imageH_ = 1;

    Frame frames_[kFrames];
    int slot_ = 0;
    uint32_t imageIndex_ = 0;

    // Present-wait thread. vkWaitForPresentKHR requires external synchronization of the
    // swapchain, so it is polled (timeout 0) under swapMutex_, which vkQueuePresentKHR also takes.
    std::thread pwThread_;
    std::mutex swapMutex_;
    std::mutex pwMutex_;
    std::condition_variable pwCv_;
    std::deque<std::pair<uint64_t, uint64_t>> pwPending_;  // (present id, swapchain generation)
    std::vector<DisplayTimingSample> pwResults_;  // frameIndex holds the present id until pollTiming maps it
    std::atomic<bool> pwStop_{false};
    uint64_t swapGen_ = 0;

    // Present ids must increase monotonically, but frame indices of repeated frames (kRepeatFrameBit) do not,
    // so presents are numbered separately and mapped back to the app's frame index.
    uint64_t presentSeq_ = 0;
    static constexpr size_t kSeqMap = 1024;
    std::array<std::pair<uint64_t, uint64_t>, kSeqMap> seqMap_{};  // (present id, frame index)
    uint64_t frameForPresent(uint64_t id) const {
        const auto& e = seqMap_[id % kSeqMap];
        return e.first == id ? e.second : 0;
    }
    int64_t lastTimingNs_ = 0;
    uint64_t timingSamples_ = 0;
};

}  // namespace vrr
