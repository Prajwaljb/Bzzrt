// Shared DXGI flip-model swap-chain handling for the D3D12 and D3D11 backends:
// tearing support, present modes, frame statistics, exclusive fullscreen, output info.
#pragma once

#include <windows.h>
#include <d3dcommon.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../common/backend.h"
#include "../common/config.h"

namespace vrr {

using Microsoft::WRL::ComPtr;

const char* hrStatus(HRESULT hr);
std::string hrHex(HRESULT hr);

class DxgiBackendBase : public Backend {
public:
    ~DxgiBackendBase() override;

    PresentOutcome present(uint64_t frameIndex) override;
    void pollTiming(std::vector<DisplayTimingSample>& out) override;
    std::vector<PresentModeInfo> presentModes() const override { return modes_; }
    int presentModeIndex() const override { return modeIdx_; }
    void setPresentMode(int index) override;
    bool supports10bit() const override { return true; }
    bool is10bit() const override { return want10_; }
    void set10bit(bool on) override { want10_ = on; }

    // Enters/leaves DXGI exclusive fullscreen at the desktop's current refresh rate.
    bool setExclusiveFullscreen(bool on);

protected:
    static constexpr UINT kBufferCount = 3;

    bool initDxgi(HWND hwnd, const Config& cfg, std::string& err);
    bool createSwapChain(IUnknown* deviceOrQueue, std::string& err);
    // Applies window resizes, 8/10-bit changes and exclusive-mode transitions. False if minimized.
    bool updateSwapChain();
    void waitFrameLatency();
    void fillInfo(BackendInfo& b, const char* api);
    ShaderParams makeShaderParams(const FrameParams& p, bool hasImage, float imageW, float imageH) const;
    bool compileShader(const char* entry, const char* target, ComPtr<ID3DBlob>& out, std::string& err) const;
    DXGI_FORMAT format() const { return is10_ ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM; }

    virtual void waitGpuIdle() = 0;
    virtual void releaseBackBuffers() = 0;
    virtual bool acquireBackBuffers() = 0;

    HWND hwnd_ = nullptr;
    ComPtr<IDXGIFactory2> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<IDXGISwapChain3> swap_;
    std::string gpuName_;
    std::vector<std::string> adapterList_;
    bool tearingSupported_ = false;
    UINT swapFlags_ = 0;
    HANDLE waitable_ = nullptr;
    UINT width_ = 0, height_ = 0;
    bool want10_ = false, is10_ = false;
    bool forceResize_ = false;
    bool exclusiveWanted_ = false, exclusiveActive_ = false;
    bool debug_ = false;

private:
    void applyColorSpace();
    std::vector<PresentModeInfo> modes_;
    int modeIdx_ = 0;
    struct PresentMapEntry {
        UINT presentCount = 0;
        uint64_t frame = 0;
    };
    std::vector<PresentMapEntry> ring_ = std::vector<PresentMapEntry>(128);
    size_t ringPos_ = 0;
    UINT lastStatsCount_ = 0;
    int64_t lastStatsNs_ = 0;
    const char* statsStatus_ = "not queried yet";
    std::string outputDesc_;
    int64_t outputDescNs_ = 0;
};

}  // namespace vrr
