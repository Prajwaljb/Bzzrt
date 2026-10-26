// Direct DRM/KMS backend: atomic page flips of CPU-drawn dumb buffers with the
// CRTC VRR_ENABLED property under our control. Runs from a text console
// (no compositor), which removes the compositor from the presentation path and
// provides kernel page-flip timestamps.
#pragma once

#include <xf86drmMode.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../common/backend.h"
#include "../common/config.h"
#include "../common/image_load.h"
#include "tty_input.h"

namespace vrr {

class KmsBackend : public Platform, public Backend {
public:
    ~KmsBackend() override;
    bool init(const Config& cfg, const ImageData& image, std::string& err);

    // Platform
    const char* name() const override { return "DRM/KMS on text console"; }
    void pollEvents(std::vector<KeyEvent>& keys) override { tty_.poll(keys); }
    bool quitRequested() const override { return tty_.quitRequested(); }
    std::vector<WindowMode> supportedWindowModes() const override { return {WindowMode::Exclusive}; }
    WindowMode windowMode() const override { return WindowMode::Exclusive; }
    void setWindowMode(WindowMode) override {}
    DisplayInfo displayInfo() override;

    // Backend
    RenderResult renderFrame(const FrameParams& p) override;
    PresentOutcome present(uint64_t frameIndex) override;
    void pollTiming(std::vector<DisplayTimingSample>& out) override;
    BackendInfo info() override;
    std::vector<PresentModeInfo> presentModes() const override;
    int presentModeIndex() const override { return modeIdx_; }
    void setPresentMode(int index) override;
    bool supports10bit() const override { return plane10_; }
    bool is10bit() const override { return want10_; }
    void set10bit(bool on) override { want10_ = on && plane10_; }

    void onFlip(uint64_t frame, int64_t ns);

private:
    struct Rect {
        int x = 0, y = 0, w = 0, h = 0;
    };
    struct Buffer {
        uint32_t handle = 0, fbId = 0, pitch = 0;
        uint64_t size = 0;
        uint8_t* map = nullptr;
        std::vector<Rect> dynamic;  // rects with time-dependent content drawn last time
        Rect overlayRect;
        uint64_t overlayVersion = ~0ull;
        bool overlayShown = false;
        bool full = true;  // needs a full redraw from base
    };
    enum Mode { kModeVrr = 0, kModeFixed = 1, kModeAsync = 2 };

    bool tryCard(const std::string& path, const Config& cfg, std::string& err);
    bool setup(const Config& cfg, std::string& err);
    uint32_t propId(uint32_t obj, uint32_t type, const char* name, uint64_t* value = nullptr) const;
    bool createBuffers(bool tenBit, std::string& err);
    void destroyBuffers();
    bool commitModeset(int buffer, bool vrr, std::string& err);
    void handleEvents(int timeoutMs);
    void waitForFlip();
    void rebuildBase();
    // Draws rect r of the frame into dst, whose top-left pixel is frame position (originX, originY).
    void compose(uint8_t* dst, size_t pitch, int originX, int originY, Rect r, const FrameParams& p);
    void compose(Buffer& b, Rect r, const FrameParams& p) { compose(b.map, b.pitch, 0, 0, r, p); }
    // Copies the overlay area from a cache that is blended once per overlay version, instead of
    // blending every pixel again for each of the three buffers. False when the cache cannot be used.
    bool blitOverlay(Buffer& b, Rect r, const FrameParams& p);
    void restore();

    TtyInput tty_;
    int fd_ = -1;
    std::string cardPath_, driverName_, connName_, monitorName_, edidRange_;
    uint32_t connId_ = 0, crtcId_ = 0, planeId_ = 0;
    int crtcIndex_ = 0;
    drmModeModeInfo mode_{};
    uint32_t modeBlob_ = 0;
    drmModeCrtc* savedCrtc_ = nullptr;
    uint64_t savedVrr_ = 0;
    bool vrrCapable_ = false;
    uint64_t maxBpc_ = 0;
    bool asyncSupported_ = false;
    bool plane10_ = false;

    // property ids
    uint32_t pConnCrtc_ = 0, pCrtcMode_ = 0, pCrtcActive_ = 0, pCrtcVrr_ = 0;
    uint32_t pFb_ = 0, pPlaneCrtc_ = 0, pSrcX_ = 0, pSrcY_ = 0, pSrcW_ = 0, pSrcH_ = 0;
    uint32_t pCrtcX_ = 0, pCrtcY_ = 0, pCrtcW_ = 0, pCrtcH_ = 0;

    int width_ = 0, height_ = 0;
    bool is10_ = false, want10_ = false;
    Buffer bufs_[3];
    int displayed_ = 0, pending_ = -1, back_ = 1;
    bool vrrActive_ = true;
    int modeIdx_ = kModeVrr;
    std::vector<uint32_t> base_;
    std::vector<uint32_t> ovCache_;
    Rect ovCacheRect_;
    uint64_t ovCacheVersion_ = ~0ull;
    int ovCachePattern_ = -1;
    bool ovCache10_ = false;
    int basePattern_ = -1;
    const ImageData* image_ = nullptr;
    std::vector<DisplayTimingSample> flips_;
    int64_t lastFlipNs_ = 0;
    std::string lastError_;
};

}  // namespace vrr
