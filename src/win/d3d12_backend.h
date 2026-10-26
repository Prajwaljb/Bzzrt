// Direct3D 12 backend.
#pragma once

#include <d3d12.h>

#include "../common/image_load.h"
#include "dxgi_backend_base.h"

namespace vrr {

class D3D12Backend : public DxgiBackendBase {
public:
    ~D3D12Backend() override;
    bool init(HWND hwnd, const Config& cfg, const ImageData& image, std::string& err);

    RenderResult renderFrame(const FrameParams& p) override;
    BackendInfo info() override;

protected:
    void waitGpuIdle() override;
    void releaseBackBuffers() override;
    bool acquireBackBuffers() override;

private:
    bool createPipeline(std::string& err);
    bool createTextures(const ImageData& image, std::string& err);
    void waitFence(UINT64 value);

    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<ID3D12CommandAllocator> alloc_[kBufferCount];
    ComPtr<ID3D12GraphicsCommandList> list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fenceEvent_ = nullptr;
    UINT64 fenceValue_ = 0;
    UINT64 slotFence_[kBufferCount] = {};

    ComPtr<ID3D12DescriptorHeap> rtvHeap_, srvHeap_;
    UINT rtvStride_ = 0;
    ComPtr<ID3D12Resource> backBuffers_[kBufferCount];
    ComPtr<ID3D12RootSignature> rootSig_;
    ComPtr<ID3D12PipelineState> pso8_, pso10_;

    ComPtr<ID3D12Resource> overlayTex_, imageTex_;
    ComPtr<ID3D12Resource> overlayUpload_[kBufferCount];
    uint8_t* overlayMapped_[kBufferCount] = {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT overlayFootprint_{};
    uint64_t overlayVersion_ = ~0ull;
    bool hasImage_ = false;
    float imageW_ = 1, imageH_ = 1;
};

}  // namespace vrr
