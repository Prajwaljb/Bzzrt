// Metal backend: renders into a CAMetalLayer and reports MTLDrawable presentedTime.
#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <mutex>
#include <string>
#include <vector>

#include "../common/backend.h"
#include "../common/config.h"
#include "../common/image_load.h"
#include "mac_platform.h"

namespace vrr {

class MetalBackend : public Backend {
public:
    ~MetalBackend() override;
    bool init(MacPlatform& platform, const Config& cfg, const ImageData& image, std::string& err);

    RenderResult renderFrame(const FrameParams& p) override;
    PresentOutcome present(uint64_t frameIndex) override;
    void pollTiming(std::vector<DisplayTimingSample>& out) override;
    BackendInfo info() override;
    std::vector<PresentModeInfo> presentModes() const override;
    int presentModeIndex() const override { return modeIdx_; }
    void setPresentMode(int index) override;
    bool supports10bit() const override { return true; }
    bool is10bit() const override { return want10_; }
    void set10bit(bool on) override { want10_ = on; }
    bool canRepeatFrames() const override { return true; }

private:
    static constexpr int kInFlight = 2;
    MTLPixelFormat format() const { return is10_ ? MTLPixelFormatBGR10A2Unorm : MTLPixelFormatBGRA8Unorm; }

    MacPlatform* platform_ = nullptr;
    CAMetalLayer* layer_ = nil;
    id<MTLDevice> device_ = nil;
    id<MTLCommandQueue> queue_ = nil;
    id<MTLLibrary> library_ = nil;
    id<MTLRenderPipelineState> pipe8_ = nil, pipe10_ = nil;
    id<MTLTexture> overlayTex_[2] = {nil, nil};
    int overlayIdx_ = 0;
    uint64_t overlayVersion_ = ~0ull;
    id<MTLTexture> imageTex_ = nil;
    id<MTLSamplerState> sampler_ = nil;
    dispatch_semaphore_t inFlight_ = nullptr;
    id<CAMetalDrawable> pending_ = nil;
    bool hasImage_ = false;
    float imageW_ = 1, imageH_ = 1;
    bool want10_ = false, is10_ = false;
    int modeIdx_ = 0;

    std::mutex timingMutex_;
    std::vector<DisplayTimingSample> timing_;
    int64_t lastTimingNs_ = 0;
    uint64_t presented_ = 0, dropped_ = 0;
};

}  // namespace vrr
