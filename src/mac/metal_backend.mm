#include "metal_backend.h"

#include <cstdio>

#include "../common/clock.h"
#include "../common/overlay.h"
#include "../common/patterns.h"
#include "pattern_metal.h"

namespace vrr {

MetalBackend::~MetalBackend() {
    // Let in-flight command buffers finish before their resources go away.
    if (queue_) {
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        [cb commit];
        [cb waitUntilCompleted];
    }
}

bool MetalBackend::init(MacPlatform& platform, const Config& cfg, const ImageData& image, std::string& err) {
    platform_ = &platform;
    layer_ = platform.layer();
    NSArray<id<MTLDevice>>* all = MTLCopyAllDevices();
    if (cfg.gpuIndex >= 0) {
        if (cfg.gpuIndex >= static_cast<int>(all.count)) return err = "no Metal device with index --gpu " +
                                                                  std::to_string(cfg.gpuIndex), false;
        device_ = all[static_cast<NSUInteger>(cfg.gpuIndex)];
    } else {
        device_ = MTLCreateSystemDefaultDevice();
    }
    if (!device_) return err = "no Metal device", false;
    queue_ = [device_ newCommandQueue];

    NSError* e = nil;
    MTLCompileOptions* opts = [MTLCompileOptions new];
    library_ = [device_ newLibraryWithSource:[NSString stringWithUTF8String:kPatternMetal] options:opts error:&e];
    if (!library_)
        return err = std::string("Metal shader compile failed: ") + (e ? e.localizedDescription.UTF8String : "?"), false;
    id<MTLFunction> vs = [library_ newFunctionWithName:@"vsMain"];
    id<MTLFunction> fs = [library_ newFunctionWithName:@"fsMain"];
    if (!vs || !fs) return err = "Metal shader entry points missing", false;
    for (int ten = 0; ten < 2; ++ten) {
        MTLRenderPipelineDescriptor* pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = vs;
        pd.fragmentFunction = fs;
        pd.colorAttachments[0].pixelFormat = ten ? MTLPixelFormatBGR10A2Unorm : MTLPixelFormatBGRA8Unorm;
        id<MTLRenderPipelineState> ps = [device_ newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!ps) return err = std::string("Metal pipeline: ") + (e ? e.localizedDescription.UTF8String : "?"), false;
        (ten ? pipe10_ : pipe8_) = ps;
    }

    MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                  width:kOverlayW
                                                                                 height:kOverlayH
                                                                              mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    for (auto& t : overlayTex_) t = [device_ newTextureWithDescriptor:td];

    hasImage_ = image.valid();
    const uint32_t placeholder = 0xFF333333u;
    const NSUInteger iw = hasImage_ ? static_cast<NSUInteger>(image.width) : 1;
    const NSUInteger ih = hasImage_ ? static_cast<NSUInteger>(image.height) : 1;
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:iw height:ih mipmapped:NO];
    td.usage = MTLTextureUsageShaderRead;
    imageTex_ = [device_ newTextureWithDescriptor:td];
    [imageTex_ replaceRegion:MTLRegionMake2D(0, 0, iw, ih)
                 mipmapLevel:0
                   withBytes:hasImage_ ? static_cast<const void*>(image.rgba.data()) : &placeholder
                 bytesPerRow:iw * 4];
    imageW_ = static_cast<float>(iw);
    imageH_ = static_cast<float>(ih);

    MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sampler_ = [device_ newSamplerStateWithDescriptor:sd];

    want10_ = is10_ = cfg.tenBit;
    layer_.device = device_;
    layer_.pixelFormat = format();
    layer_.framebufferOnly = YES;
    layer_.maximumDrawableCount = 3;
    layer_.displaySyncEnabled = YES;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    layer_.colorspace = cs;
    CGColorSpaceRelease(cs);
    inFlight_ = dispatch_semaphore_create(kInFlight);
    return true;
}

RenderResult MetalBackend::renderFrame(const FrameParams& p) {
    @autoreleasepool {
        if (want10_ != is10_) {
            is10_ = want10_;
            layer_.pixelFormat = format();
        }
        const CGSize size = layer_.drawableSize;
        if (size.width < 1 || size.height < 1) return RenderResult::Skipped;
        if (dispatch_semaphore_wait(inFlight_, dispatch_time(DISPATCH_TIME_NOW, 1000000000LL)) != 0)
            return RenderResult::Skipped;
        id<CAMetalDrawable> drawable = [layer_ nextDrawable];
        if (!drawable) {
            dispatch_semaphore_signal(inFlight_);
            return RenderResult::Skipped;
        }

        // Two overlay textures: the one being replaced is never the one an in-flight frame still reads.
        if (p.overlayPixels && p.overlayVersion != overlayVersion_) {
            overlayIdx_ ^= 1;
            [overlayTex_[overlayIdx_] replaceRegion:MTLRegionMake2D(0, 0, kOverlayW, kOverlayH)
                                        mipmapLevel:0
                                          withBytes:p.overlayPixels
                                        bytesPerRow:kOverlayW * 4];
            overlayVersion_ = p.overlayVersion;
        }

        ShaderParams sp{};
        const int w = static_cast<int>(drawable.texture.width), h = static_cast<int>(drawable.texture.height);
        sp.resolution[0] = static_cast<float>(w);
        sp.resolution[1] = static_cast<float>(h);
        sp.overlayPos[0] = static_cast<float>(p.overlayX);
        sp.overlayPos[1] = static_cast<float>(p.overlayY);
        sp.overlaySize[0] = static_cast<float>(p.overlayW);
        sp.overlaySize[1] = static_cast<float>(p.overlayH);
        sp.moveX = static_cast<float>(moveBarX(p.patternTime, w));
        sp.pattern = static_cast<uint32_t>(p.pattern);
        sp.flags = (p.overlayVisible ? kFlagOverlay : 0u) | (p.frameCounter ? kFlagCounter : 0u) |
                   (hasImage_ ? kFlagImage : 0u);
        sp.frame = static_cast<uint32_t>(p.frameIndex);
        sp.imageSize[0] = imageW_;
        sp.imageSize[1] = imageH_;

        MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:is10_ ? pipe10_ : pipe8_];
        [enc setFragmentBytes:&sp length:sizeof sp atIndex:0];
        [enc setFragmentTexture:overlayTex_[overlayIdx_] atIndex:0];
        [enc setFragmentTexture:imageTex_ atIndex:1];
        [enc setFragmentSamplerState:sampler_ atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];
        dispatch_semaphore_t sem = inFlight_;
        [cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
          (void)done;
          dispatch_semaphore_signal(sem);
        }];
        [cb commit];
        pending_ = drawable;
    }
    return RenderResult::Rendered;
}

PresentOutcome MetalBackend::present(uint64_t frameIndex) {
    PresentOutcome o;
    if (!pending_) {
        o.status = "no drawable";
        o.ok = false;
        return o;
    }
    @autoreleasepool {
        id<CAMetalDrawable> d = pending_;
        pending_ = nil;
        [d addPresentedHandler:^(id<MTLDrawable> dr) {
          // presentedTime is in the CACurrentMediaTime / mach_absolute_time domain, like nowNs(); 0 = dropped.
          const CFTimeInterval t = dr.presentedTime;
          std::lock_guard<std::mutex> lk(timingMutex_);
          if (t > 0) {
              timing_.push_back({frameIndex, static_cast<int64_t>(t * 1e9)});
              ++presented_;
          } else {
              ++dropped_;
          }
        }];
        // An empty command buffer on the same queue presents after the frame's rendering, right now.
        id<MTLCommandBuffer> cb = [queue_ commandBuffer];
        [cb presentDrawable:d];
        [cb commit];
    }
    return o;
}

void MetalBackend::pollTiming(std::vector<DisplayTimingSample>& out) {
    std::lock_guard<std::mutex> lk(timingMutex_);
    if (!timing_.empty()) lastTimingNs_ = nowNs();
    out.insert(out.end(), timing_.begin(), timing_.end());
    timing_.clear();
}

std::vector<PresentModeInfo> MetalBackend::presentModes() const {
    return {{"vsync", "CAMetalLayer displaySyncEnabled=YES (VRR in fullscreen on Adaptive-Sync / ProMotion)", true},
            {"immediate", "CAMetalLayer displaySyncEnabled=NO (tearing)", false}};
}

void MetalBackend::setPresentMode(int index) {
    if (index < 0 || index > 1) return;
    modeIdx_ = index;
    layer_.displaySyncEnabled = index == 0;
}

BackendInfo MetalBackend::info() {
    BackendInfo b;
    b.api = "Metal / CAMetalLayer";
    b.gpu = device_.name.UTF8String ? device_.name.UTF8String : "?";
    b.swapFormat = is10_ ? "MTLPixelFormatBGR10A2Unorm (10-bit SDR sRGB)" : "MTLPixelFormatBGRA8Unorm (8-bit SDR sRGB)";
    b.presentMode = presentModes()[modeIdx_].name;
    b.tearing = "displaySyncEnabled=NO presents without waiting for vblank";
    b.timingSource = "MTLDrawable presentedTime";
    b.timingAvailable = lastTimingNs_ != 0 && nowNs() - lastTimingNs_ < 2000000000LL;
    b.width = static_cast<int>(layer_.drawableSize.width);
    b.height = static_cast<int>(layer_.drawableSize.height);
    {
        std::lock_guard<std::mutex> lk(timingMutex_);
        char buf[128];
        std::snprintf(buf, sizeof buf, "drawables presented %llu, dropped %llu",
                      static_cast<unsigned long long>(presented_), static_cast<unsigned long long>(dropped_));
        b.notes.push_back(buf);
    }
    double lo = 0, hi = 0;
    platform_->screenVrrRange(lo, hi);
    if (hi > 0)
        b.setup.push_back({"macOS reports variable refresh on this display (" + std::to_string(static_cast<int>(lo)) +
                               "-" + std::to_string(static_cast<int>(hi)) + " Hz)",
                           false});
    else
        b.setup.push_back({"macOS reports a fixed refresh rate here. For an Adaptive-Sync monitor choose "
                           "\"Variable\" under System Settings > Displays > Refresh Rate (macOS 14+ on Apple silicon).",
                           true});
    if (platform_->windowMode() == WindowMode::Windowed)
        b.setup.push_back({"macOS only uses variable refresh for fullscreen apps: press F.", true});
    return b;
}

}  // namespace vrr
