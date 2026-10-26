#include "d3d11_backend.h"

#include <cstring>

#include "../common/overlay.h"

namespace vrr {

D3D11Backend::~D3D11Backend() {
    if (ctx_) {
        ctx_->ClearState();
        ctx_->Flush();
    }
    if (swap_) swap_->SetFullscreenState(FALSE, nullptr);
}

bool D3D11Backend::init(HWND hwnd, const Config& cfg, const ImageData& image, std::string& err) {
    if (!initDxgi(hwnd, cfg, err)) return false;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | (cfg.debug ? D3D11_CREATE_DEVICE_DEBUG : 0);
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    HRESULT hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels, 2,
                                   D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    if (hr == E_INVALIDARG)  // runtimes without 11_1
        hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels + 1, 1,
                               D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    if (FAILED(hr) && cfg.debug) {
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = D3D11CreateDevice(adapter_.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, flags, levels + 1, 1,
                               D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    }
    if (FAILED(hr)) return err = "D3D11CreateDevice failed " + hrHex(hr), false;
    if (!createSwapChain(device_.Get(), err)) return false;
    if (!acquireBackBuffers()) return err = "cannot create render target view", false;

    ComPtr<ID3DBlob> vsb, psb;
    if (!compileShader("VSMain", "vs_5_0", vsb, err) || !compileShader("PSMain", "ps_5_0", psb, err)) return false;
    device_->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &vs_);
    device_->CreatePixelShader(psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &ps_);

    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(ShaderParams);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &cb_))) return err = "constant buffer", false;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = kOverlayW;
    td.Height = kOverlayH;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateTexture2D(&td, nullptr, &overlayTex_))) return err = "overlay texture", false;
    device_->CreateShaderResourceView(overlayTex_.Get(), nullptr, &overlaySrv_);

    hasImage_ = image.valid();
    const uint32_t placeholder = 0xFF333333u;
    td.Width = hasImage_ ? static_cast<UINT>(image.width) : 1;
    td.Height = hasImage_ ? static_cast<UINT>(image.height) : 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.CPUAccessFlags = 0;
    imageW_ = static_cast<float>(td.Width);
    imageH_ = static_cast<float>(td.Height);
    D3D11_SUBRESOURCE_DATA init{};
    init.pSysMem = hasImage_ ? static_cast<const void*>(image.rgba.data()) : &placeholder;
    init.SysMemPitch = td.Width * 4;
    if (FAILED(device_->CreateTexture2D(&td, &init, &imageTex_))) return err = "image texture", false;
    device_->CreateShaderResourceView(imageTex_.Get(), nullptr, &imageSrv_);

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    device_->CreateSamplerState(&sd, &sampler_);
    return true;
}

void D3D11Backend::waitGpuIdle() {
    ctx_->ClearState();
    ctx_->Flush();
}

void D3D11Backend::releaseBackBuffers() { rtv_.Reset(); }

bool D3D11Backend::acquireBackBuffers() {
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(&bb)))) return false;
    return SUCCEEDED(device_->CreateRenderTargetView(bb.Get(), nullptr, &rtv_));
}

RenderResult D3D11Backend::renderFrame(const FrameParams& p) {
    if (!updateSwapChain()) return RenderResult::Skipped;
    waitFrameLatency();

    D3D11_MAPPED_SUBRESOURCE m{};
    if (p.overlayPixels && p.overlayVersion != overlayVersion_ &&
        SUCCEEDED(ctx_->Map(overlayTex_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        for (int y = 0; y < kOverlayH; ++y)
            std::memcpy(static_cast<uint8_t*>(m.pData) + static_cast<size_t>(y) * m.RowPitch,
                        p.overlayPixels + static_cast<size_t>(y) * kOverlayW, static_cast<size_t>(kOverlayW) * 4);
        ctx_->Unmap(overlayTex_.Get(), 0);
        overlayVersion_ = p.overlayVersion;
    }
    const ShaderParams sp = makeShaderParams(p, hasImage_, imageW_, imageH_);
    if (SUCCEEDED(ctx_->Map(cb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        std::memcpy(m.pData, &sp, sizeof sp);
        ctx_->Unmap(cb_.Get(), 0);
    }

    // Flip-model presents unbind the render target, so everything is re-bound each frame.
    ID3D11RenderTargetView* rtv = rtv_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    ctx_->RSSetViewports(1, &vp);
    ctx_->IASetInputLayout(nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11Buffer* cb = cb_.Get();
    ctx_->VSSetConstantBuffers(0, 1, &cb);
    ctx_->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srvs[2] = {overlaySrv_.Get(), imageSrv_.Get()};
    ctx_->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* s = sampler_.Get();
    ctx_->PSSetSamplers(0, 1, &s);
    ctx_->Draw(3, 0);
    return RenderResult::Rendered;
}

BackendInfo D3D11Backend::info() {
    BackendInfo b;
    fillInfo(b, "Direct3D 11 (fallback)");
    return b;
}

}  // namespace vrr
