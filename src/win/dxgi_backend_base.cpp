#include "dxgi_backend_base.h"

#include <d3dcompiler.h>

#include <cstdio>
#include <cstring>

#include "../common/clock.h"
#include "../common/patterns.h"
#include "pattern_hlsl.h"
#include "win_platform.h"

namespace vrr {

const char* hrStatus(HRESULT hr) {
    switch (hr) {
        case S_OK: return "S_OK";
        case DXGI_STATUS_OCCLUDED: return "DXGI_STATUS_OCCLUDED";
        case DXGI_STATUS_MODE_CHANGED: return "DXGI_STATUS_MODE_CHANGED";
        case DXGI_ERROR_DEVICE_REMOVED: return "DXGI_ERROR_DEVICE_REMOVED";
        case DXGI_ERROR_DEVICE_RESET: return "DXGI_ERROR_DEVICE_RESET";
        case DXGI_ERROR_DEVICE_HUNG: return "DXGI_ERROR_DEVICE_HUNG";
        case DXGI_ERROR_INVALID_CALL: return "DXGI_ERROR_INVALID_CALL";
        case DXGI_ERROR_FRAME_STATISTICS_DISJOINT: return "DXGI_ERROR_FRAME_STATISTICS_DISJOINT";
        case DXGI_ERROR_WAS_STILL_DRAWING: return "DXGI_ERROR_WAS_STILL_DRAWING";
        case DXGI_ERROR_NOT_CURRENTLY_AVAILABLE: return "DXGI_ERROR_NOT_CURRENTLY_AVAILABLE";
        case DXGI_ERROR_UNSUPPORTED: return "DXGI_ERROR_UNSUPPORTED";
        case E_INVALIDARG: return "E_INVALIDARG";
        case E_FAIL: return "E_FAIL";
        default: return SUCCEEDED(hr) ? "S_(other)" : "E_(other)";
    }
}

std::string hrHex(HRESULT hr) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

static const char* colorSpaceName(DXGI_COLOR_SPACE_TYPE cs) {
    switch (cs) {
        case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709: return "G22_P709 (SDR)";
        case DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020: return "G2084_P2020 (HDR10)";
        case DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709: return "G10_P709 (scRGB)";
        default: return "other";
    }
}

DxgiBackendBase::~DxgiBackendBase() {
    if (swap_) swap_->SetFullscreenState(FALSE, nullptr);
    if (waitable_) CloseHandle(waitable_);
}

bool DxgiBackendBase::initDxgi(HWND hwnd, const Config& cfg, std::string& err) {
    hwnd_ = hwnd;
    debug_ = cfg.debug;
    want10_ = is10_ = cfg.tenBit;
    HRESULT hr = CreateDXGIFactory2(debug_ ? DXGI_CREATE_FACTORY_DEBUG : 0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr) && debug_) hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
    if (FAILED(hr)) return err = "CreateDXGIFactory2 failed " + hrHex(hr), false;

    ComPtr<IDXGIFactory5> f5;
    if (SUCCEEDED(factory_.As(&f5))) {
        BOOL allow = FALSE;
        if (SUCCEEDED(f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allow, sizeof allow)))
            tearingSupported_ = allow != FALSE;
    }

    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> a;
        if (factory_->EnumAdapters1(i, &a) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        const std::string name = narrow(d.Description);
        adapterList_.push_back("[" + std::to_string(i) + "] " + name);
        if ((cfg.gpuIndex < 0 && !adapter_) || static_cast<int>(i) == cfg.gpuIndex) {
            adapter_ = a;
            gpuName_ = name;
        }
    }
    if (!adapter_) return err = "no hardware DXGI adapter (check --gpu)", false;
    std::printf("DXGI adapters:\n");
    for (const auto& a : adapterList_) std::printf("  %s\n", a.c_str());
    std::printf("DXGI_FEATURE_PRESENT_ALLOW_TEARING: %s\n", tearingSupported_ ? "TRUE" : "FALSE");

    modes_.push_back({"vsync", "SyncInterval=1 (vsync; VRR works in independent flip)", true});
    if (tearingSupported_)
        modes_.push_back({"tearing", "SyncInterval=0 + DXGI_PRESENT_ALLOW_TEARING", false});
    modes_.push_back({"nosync", "SyncInterval=0 without ALLOW_TEARING (no tearing when composed)", false});
    return true;
}

void DxgiBackendBase::applyColorSpace() {
    // SDR in both 8- and 10-bit; HDR is never enabled by this tool.
    UINT support = 0;
    const DXGI_COLOR_SPACE_TYPE cs = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (SUCCEEDED(swap_->CheckColorSpaceSupport(cs, &support)) &&
        (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT))
        swap_->SetColorSpace1(cs);
}

bool DxgiBackendBase::createSwapChain(IUnknown* deviceOrQueue, std::string& err) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    width_ = static_cast<UINT>(rc.right - rc.left > 0 ? rc.right - rc.left : 1);
    height_ = static_cast<UINT>(rc.bottom - rc.top > 0 ? rc.bottom - rc.top : 1);
    swapFlags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT |
                 (tearingSupported_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = width_;
    sd.Height = height_;
    sd.Format = format();
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kBufferCount;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = swapFlags_;
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = factory_->CreateSwapChainForHwnd(deviceOrQueue, hwnd_, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr) && is10_) {
        std::printf("10-bit swap chain creation failed (%s); falling back to 8-bit\n", hrHex(hr).c_str());
        want10_ = is10_ = false;
        sd.Format = format();
        hr = factory_->CreateSwapChainForHwnd(deviceOrQueue, hwnd_, &sd, nullptr, nullptr, &sc1);
    }
    if (FAILED(hr)) return err = "CreateSwapChainForHwnd failed " + hrHex(hr), false;
    factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    if (FAILED(sc1.As(&swap_))) return err = "IDXGISwapChain3 not available (Windows 10+ required)", false;
    swap_->SetMaximumFrameLatency(1);
    waitable_ = swap_->GetFrameLatencyWaitableObject();
    applyColorSpace();
    return true;
}

bool DxgiBackendBase::updateSwapChain() {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    const UINT w = static_cast<UINT>(rc.right - rc.left), h = static_cast<UINT>(rc.bottom - rc.top);
    if (w == 0 || h == 0 || IsIconic(hwnd_)) return false;
    if (w == width_ && h == height_ && want10_ == is10_ && !forceResize_) return true;

    waitGpuIdle();
    releaseBackBuffers();
    is10_ = want10_;
    HRESULT hr = swap_->ResizeBuffers(kBufferCount, w, h, format(), swapFlags_);
    if (FAILED(hr) && is10_) {
        std::printf("ResizeBuffers with 10-bit format failed (%s); using 8-bit\n", hrHex(hr).c_str());
        want10_ = is10_ = false;
        hr = swap_->ResizeBuffers(kBufferCount, w, h, format(), swapFlags_);
    }
    if (FAILED(hr)) {
        std::fprintf(stderr, "ResizeBuffers failed %s\n", hrHex(hr).c_str());
        return false;
    }
    width_ = w;
    height_ = h;
    forceResize_ = false;
    applyColorSpace();
    return acquireBackBuffers();
}

void DxgiBackendBase::waitFrameLatency() {
    if (waitable_) WaitForSingleObjectEx(waitable_, 1000, TRUE);
}

bool DxgiBackendBase::setExclusiveFullscreen(bool on) {
    if (!swap_) return false;
    if (!on) {
        swap_->SetFullscreenState(FALSE, nullptr);
        exclusiveWanted_ = exclusiveActive_ = false;
        forceResize_ = true;
        return true;
    }
    ComPtr<IDXGIOutput> out;
    if (FAILED(swap_->GetContainingOutput(&out))) return false;
    DXGI_OUTPUT_DESC od{};
    out->GetDesc(&od);
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    if (EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm)) {
        // Keep the desktop resolution and refresh rate instead of letting DXGI pick a mode.
        DXGI_MODE_DESC want{};
        want.Width = dm.dmPelsWidth;
        want.Height = dm.dmPelsHeight;
        want.RefreshRate.Numerator = dm.dmDisplayFrequency;
        want.RefreshRate.Denominator = 1;
        want.Format = format();
        DXGI_MODE_DESC closest{};
        if (SUCCEEDED(out->FindClosestMatchingMode(&want, &closest, nullptr))) swap_->ResizeTarget(&closest);
    }
    const HRESULT hr = swap_->SetFullscreenState(TRUE, out.Get());
    if (FAILED(hr)) {
        std::printf("SetFullscreenState(TRUE) failed: %s\n", hrStatus(hr));
        return false;
    }
    exclusiveWanted_ = exclusiveActive_ = true;
    forceResize_ = true;
    return true;
}

void DxgiBackendBase::setPresentMode(int index) {
    if (index >= 0 && index < static_cast<int>(modes_.size())) modeIdx_ = index;
}

PresentOutcome DxgiBackendBase::present(uint64_t frameIndex) {
    UINT sync = 1, flags = 0;
    const std::string& id = modes_[modeIdx_].id;
    if (id == "tearing") {
        sync = 0;
        // ALLOW_TEARING is invalid in exclusive fullscreen, where SyncInterval=0 tears by itself.
        if (!exclusiveActive_) flags = DXGI_PRESENT_ALLOW_TEARING;
    } else if (id == "nosync") {
        sync = 0;
    }
    const HRESULT hr = swap_->Present(sync, flags);
    UINT pc = 0;
    if (SUCCEEDED(swap_->GetLastPresentCount(&pc))) ring_[ringPos_++ % ring_.size()] = {pc, frameIndex};

    if (exclusiveWanted_) {
        BOOL fs = FALSE;
        swap_->GetFullscreenState(&fs, nullptr);
        if ((fs != FALSE) != exclusiveActive_) {
            exclusiveActive_ = fs != FALSE;
            forceResize_ = true;
        }
    }
    PresentOutcome o;
    o.status = hrStatus(hr);
    o.ok = SUCCEEDED(hr);
    o.fatal = hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG;
    return o;
}

void DxgiBackendBase::pollTiming(std::vector<DisplayTimingSample>& out) {
    DXGI_FRAME_STATISTICS st{};
    const HRESULT hr = swap_->GetFrameStatistics(&st);
    statsStatus_ = hrStatus(hr);
    if (FAILED(hr) || st.PresentCount == lastStatsCount_ || st.SyncQPCTime.QuadPart == 0) return;
    lastStatsCount_ = st.PresentCount;
    for (const auto& e : ring_) {
        if (e.frame && e.presentCount == st.PresentCount) {
            out.push_back({e.frame, qpcToNs(st.SyncQPCTime.QuadPart)});
            lastStatsNs_ = nowNs();
            break;
        }
    }
}

ShaderParams DxgiBackendBase::makeShaderParams(const FrameParams& p, bool hasImage, float imageW, float imageH) const {
    ShaderParams sp{};
    sp.resolution[0] = static_cast<float>(width_);
    sp.resolution[1] = static_cast<float>(height_);
    sp.overlayPos[0] = static_cast<float>(p.overlayX);
    sp.overlayPos[1] = static_cast<float>(p.overlayY);
    sp.overlaySize[0] = static_cast<float>(p.overlayW);
    sp.overlaySize[1] = static_cast<float>(p.overlayH);
    sp.moveX = static_cast<float>(moveBarX(p.patternTime, static_cast<int>(width_)));
    sp.pattern = static_cast<uint32_t>(p.pattern);
    sp.flags = (p.overlayVisible ? kFlagOverlay : 0u) | (p.frameCounter ? kFlagCounter : 0u) |
               (hasImage ? kFlagImage : 0u);
    sp.frame = static_cast<uint32_t>(p.frameIndex);
    sp.imageSize[0] = imageW;
    sp.imageSize[1] = imageH;
    return sp;
}

bool DxgiBackendBase::compileShader(const char* entry, const char* target, ComPtr<ID3DBlob>& out,
                                    std::string& err) const {
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(kPatternHlsl, std::strlen(kPatternHlsl), "pattern.hlsl", nullptr, nullptr, entry,
                                  target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors);
    if (FAILED(hr)) {
        err = std::string("D3DCompile ") + entry + " failed: " +
              (errors ? static_cast<const char*>(errors->GetBufferPointer()) : hrHex(hr));
        return false;
    }
    return true;
}

void DxgiBackendBase::fillInfo(BackendInfo& b, const char* api) {
    b.api = std::string(api) + " / DXGI flip model (FLIP_DISCARD, " + std::to_string(kBufferCount) + " buffers)";
    b.gpu = gpuName_;
    b.swapFormat = is10_ ? "DXGI_FORMAT_R10G10B10A2_UNORM (10-bit SDR G22_P709)"
                         : "DXGI_FORMAT_B8G8R8A8_UNORM (8-bit SDR G22_P709)";
    b.presentMode = modes_[modeIdx_].name + (exclusiveActive_ ? " [exclusive fullscreen]" : "");
    b.tearing = tearingSupported_
                    ? "DXGI_FEATURE_PRESENT_ALLOW_TEARING=TRUE; swap chain has DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING"
                    : "DXGI_FEATURE_PRESENT_ALLOW_TEARING=FALSE (no tearing/VRR for flip-model windows)";
    b.timingSource = std::string("DXGI GetFrameStatistics SyncQPCTime (last: ") + statsStatus_ + ")";
    b.timingAvailable = lastStatsNs_ != 0 && nowNs() - lastStatsNs_ < 2000000000LL;
    b.width = static_cast<int>(width_);
    b.height = static_cast<int>(height_);

    const int64_t now = nowNs();
    if (now - outputDescNs_ > 2000000000LL) {
        outputDescNs_ = now;
        outputDesc_.clear();
        ComPtr<IDXGIOutput> out;
        ComPtr<IDXGIOutput6> out6;
        if (SUCCEEDED(swap_->GetContainingOutput(&out)) && SUCCEEDED(out.As(&out6))) {
            DXGI_OUTPUT_DESC1 d{};
            if (SUCCEEDED(out6->GetDesc1(&d))) {
                char buf[256];
                std::snprintf(buf, sizeof buf, "DXGI output %s: BitsPerColor %u, ColorSpace %s",
                              narrow(d.DeviceName).c_str(), d.BitsPerColor, colorSpaceName(d.ColorSpace));
                outputDesc_ = buf;
            }
        }
    }
    if (!outputDesc_.empty()) b.notes.push_back(outputDesc_);
    b.notes.push_back("Use PresentMon: VRR needs 'Hardware: Independent Flip' (or Composed: Independent Flip).");
    if (adapterList_.size() > 1) {
        std::string g = "Adapters:";
        for (const auto& a : adapterList_) g += " " + a;
        b.notes.push_back(g + "  (--gpu N)");
    }
}

}  // namespace vrr
