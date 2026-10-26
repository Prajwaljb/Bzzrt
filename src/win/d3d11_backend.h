// Direct3D 11 fallback backend (same DXGI flip-model swap chain as D3D12).
#pragma once

#include <d3d11.h>

#include "../common/image_load.h"
#include "dxgi_backend_base.h"

namespace vrr {

class D3D11Backend : public DxgiBackendBase {
public:
    ~D3D11Backend() override;
    bool init(HWND hwnd, const Config& cfg, const ImageData& image, std::string& err);

    RenderResult renderFrame(const FrameParams& p) override;
    BackendInfo info() override;

protected:
    void waitGpuIdle() override;
    void releaseBackBuffers() override;
    bool acquireBackBuffers() override;

private:
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> cb_;
    ComPtr<ID3D11Texture2D> overlayTex_, imageTex_;
    ComPtr<ID3D11ShaderResourceView> overlaySrv_, imageSrv_;
    ComPtr<ID3D11SamplerState> sampler_;
    uint64_t overlayVersion_ = ~0ull;
    bool hasImage_ = false;
    float imageW_ = 1, imageH_ = 1;
};

}  // namespace vrr
