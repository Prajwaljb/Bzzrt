#include "d3d12_backend.h"

#include <climits>
#include <cstdio>
#include <cstring>

#include "../common/overlay.h"

namespace vrr {

namespace {

D3D12_HEAP_PROPERTIES heapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES h{};
    h.Type = type;
    h.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    h.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
    h.CreationNodeMask = 1;
    h.VisibleNodeMask = 1;
    return h;
}

D3D12_RESOURCE_DESC bufferDesc(UINT64 size) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = 1;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return d;
}

D3D12_RESOURCE_DESC textureDesc(UINT w, UINT h) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w;
    d.Height = h;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    return d;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

void copyBufferToTexture(ID3D12GraphicsCommandList* list, ID3D12Resource* tex, ID3D12Resource* buf,
                         const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp) {
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = tex;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = buf;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
}

}  // namespace

D3D12Backend::~D3D12Backend() {
    if (queue_ && fence_) waitGpuIdle();
    if (swap_) swap_->SetFullscreenState(FALSE, nullptr);
    if (fenceEvent_) CloseHandle(fenceEvent_);
}

bool D3D12Backend::init(HWND hwnd, const Config& cfg, const ImageData& image, std::string& err) {
    if (cfg.debug) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    if (!initDxgi(hwnd, cfg, err)) return false;
    HRESULT hr = D3D12CreateDevice(adapter_.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device_));
    if (FAILED(hr)) return err = "D3D12CreateDevice failed " + hrHex(hr), false;

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(device_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_)))) return err = "CreateCommandQueue failed", false;
    if (!createSwapChain(queue_.Get(), err)) return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = kBufferCount;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap_)))) return err = "RTV heap", false;
    rtvStride_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 2;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(device_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap_)))) return err = "SRV heap", false;

    for (UINT i = 0; i < kBufferCount; ++i)
        if (FAILED(device_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_[i]))))
            return err = "CreateCommandAllocator failed", false;
    if (FAILED(device_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_[0].Get(), nullptr,
                                          IID_PPV_ARGS(&list_))))
        return err = "CreateCommandList failed", false;
    list_->Close();
    if (FAILED(device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return err = "CreateFence", false;
    fenceEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    if (!acquireBackBuffers()) return err = "cannot get swap-chain buffers", false;
    if (!createPipeline(err)) return false;
    return createTextures(image, err);
}

void D3D12Backend::waitFence(UINT64 value) {
    if (value && fence_->GetCompletedValue() < value) {
        fence_->SetEventOnCompletion(value, fenceEvent_);
        WaitForSingleObject(fenceEvent_, INFINITE);
    }
}

void D3D12Backend::waitGpuIdle() {
    queue_->Signal(fence_.Get(), ++fenceValue_);
    waitFence(fenceValue_);
}

void D3D12Backend::releaseBackBuffers() {
    for (auto& b : backBuffers_) b.Reset();
}

bool D3D12Backend::acquireBackBuffers() {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < kBufferCount; ++i) {
        if (FAILED(swap_->GetBuffer(i, IID_PPV_ARGS(&backBuffers_[i])))) return false;
        device_->CreateRenderTargetView(backBuffers_[i].Get(), nullptr, h);
        h.ptr += rtvStride_;
    }
    return true;
}

bool D3D12Backend::createPipeline(std::string& err) {
    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 2;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = sizeof(ShaderParams) / 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_STATIC_SAMPLER_DESC samp{};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    samp.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;
    ComPtr<ID3DBlob> blob, errors;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors)))
        return err = "D3D12SerializeRootSignature failed", false;
    if (FAILED(device_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                            IID_PPV_ARGS(&rootSig_))))
        return err = "CreateRootSignature failed", false;

    ComPtr<ID3DBlob> vs, ps;
    if (!compileShader("VSMain", "vs_5_0", vs, err) || !compileShader("PSMain", "ps_5_0", ps, err)) return false;

    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
    pd.pRootSignature = rootSig_.Get();
    pd.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pd.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    D3D12_RENDER_TARGET_BLEND_DESC rtb{};
    rtb.SrcBlend = D3D12_BLEND_ONE;
    rtb.DestBlend = D3D12_BLEND_ZERO;
    rtb.BlendOp = D3D12_BLEND_OP_ADD;
    rtb.SrcBlendAlpha = D3D12_BLEND_ONE;
    rtb.DestBlendAlpha = D3D12_BLEND_ZERO;
    rtb.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    rtb.LogicOp = D3D12_LOGIC_OP_NOOP;
    rtb.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.BlendState.RenderTarget[0] = rtb;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pd.RasterizerState.DepthClipEnable = TRUE;
    pd.DepthStencilState.DepthEnable = FALSE;
    pd.DepthStencilState.StencilEnable = FALSE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1;
    pd.SampleDesc.Count = 1;
    pd.RTVFormats[0] = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (FAILED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso8_)))) return err = "PSO (8-bit) failed", false;
    pd.RTVFormats[0] = DXGI_FORMAT_R10G10B10A2_UNORM;
    if (FAILED(device_->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso10_)))) return err = "PSO (10-bit) failed", false;
    return true;
}

bool D3D12Backend::createTextures(const ImageData& image, std::string& err) {
    const D3D12_HEAP_PROPERTIES defHeap = heapProps(D3D12_HEAP_TYPE_DEFAULT);
    const D3D12_HEAP_PROPERTIES upHeap = heapProps(D3D12_HEAP_TYPE_UPLOAD);

    // Overlay texture + one persistently mapped upload buffer per frame slot.
    D3D12_RESOURCE_DESC od = textureDesc(kOverlayW, kOverlayH);
    if (FAILED(device_->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &od,
                                                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                                                IID_PPV_ARGS(&overlayTex_))))
        return err = "overlay texture", false;
    UINT64 total = 0;
    device_->GetCopyableFootprints(&od, 0, 1, 0, &overlayFootprint_, nullptr, nullptr, &total);
    const D3D12_RESOURCE_DESC ub = bufferDesc(total);
    for (UINT i = 0; i < kBufferCount; ++i) {
        if (FAILED(device_->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &ub,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(&overlayUpload_[i]))))
            return err = "overlay upload buffer", false;
        D3D12_RANGE none{0, 0};
        void* p = nullptr;
        overlayUpload_[i]->Map(0, &none, &p);
        overlayMapped_[i] = static_cast<uint8_t*>(p);
    }

    // User image (or a 1x1 placeholder), uploaded once.
    hasImage_ = image.valid();
    const UINT iw = hasImage_ ? static_cast<UINT>(image.width) : 1;
    const UINT ih = hasImage_ ? static_cast<UINT>(image.height) : 1;
    imageW_ = static_cast<float>(iw);
    imageH_ = static_cast<float>(ih);
    const uint32_t placeholder = 0xFF333333u;
    const uint32_t* pixels = hasImage_ ? image.rgba.data() : &placeholder;
    D3D12_RESOURCE_DESC id = textureDesc(iw, ih);
    if (FAILED(device_->CreateCommittedResource(&defHeap, D3D12_HEAP_FLAG_NONE, &id, D3D12_RESOURCE_STATE_COPY_DEST,
                                                nullptr, IID_PPV_ARGS(&imageTex_))))
        return err = "image texture", false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    device_->GetCopyableFootprints(&id, 0, 1, 0, &fp, nullptr, nullptr, &total);
    const D3D12_RESOURCE_DESC ib = bufferDesc(total);
    ComPtr<ID3D12Resource> upload;
    if (FAILED(device_->CreateCommittedResource(&upHeap, D3D12_HEAP_FLAG_NONE, &ib, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(&upload))))
        return err = "image upload buffer", false;
    uint8_t* mapped = nullptr;
    D3D12_RANGE none{0, 0};
    upload->Map(0, &none, reinterpret_cast<void**>(&mapped));
    for (UINT y = 0; y < ih; ++y)
        std::memcpy(mapped + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, pixels + static_cast<size_t>(y) * iw,
                    static_cast<size_t>(iw) * 4);
    upload->Unmap(0, nullptr);

    alloc_[0]->Reset();
    list_->Reset(alloc_[0].Get(), nullptr);
    copyBufferToTexture(list_.Get(), imageTex_.Get(), upload.Get(), fp);
    const D3D12_RESOURCE_BARRIER b =
        transition(imageTex_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    list_->ResourceBarrier(1, &b);
    list_->Close();
    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    waitGpuIdle();

    const UINT srvStride = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap_->GetCPUDescriptorHandleForHeapStart();
    device_->CreateShaderResourceView(overlayTex_.Get(), nullptr, h);
    h.ptr += srvStride;
    device_->CreateShaderResourceView(imageTex_.Get(), nullptr, h);
    return true;
}

RenderResult D3D12Backend::renderFrame(const FrameParams& p) {
    if (!updateSwapChain()) return RenderResult::Skipped;
    waitFrameLatency();
    const UINT bi = swap_->GetCurrentBackBufferIndex();
    waitFence(slotFence_[bi]);
    alloc_[bi]->Reset();
    list_->Reset(alloc_[bi].Get(), is10_ ? pso10_.Get() : pso8_.Get());

    if (p.overlayPixels && p.overlayVersion != overlayVersion_) {
        uint8_t* dst = overlayMapped_[bi] + overlayFootprint_.Offset;
        for (int y = 0; y < kOverlayH; ++y)
            std::memcpy(dst + static_cast<size_t>(y) * overlayFootprint_.Footprint.RowPitch,
                        p.overlayPixels + static_cast<size_t>(y) * kOverlayW, static_cast<size_t>(kOverlayW) * 4);
        D3D12_RESOURCE_BARRIER b = transition(overlayTex_.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                              D3D12_RESOURCE_STATE_COPY_DEST);
        list_->ResourceBarrier(1, &b);
        copyBufferToTexture(list_.Get(), overlayTex_.Get(), overlayUpload_[bi].Get(), overlayFootprint_);
        b = transition(overlayTex_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        list_->ResourceBarrier(1, &b);
        overlayVersion_ = p.overlayVersion;
    }

    D3D12_RESOURCE_BARRIER b =
        transition(backBuffers_[bi].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
    list_->ResourceBarrier(1, &b);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap_->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(bi) * rtvStride_;
    list_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    const D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width_), static_cast<float>(height_), 0.0f, 1.0f};
    const D3D12_RECT sc{0, 0, static_cast<LONG>(width_), static_cast<LONG>(height_)};
    list_->RSSetViewports(1, &vp);
    list_->RSSetScissorRects(1, &sc);
    list_->SetGraphicsRootSignature(rootSig_.Get());
    ID3D12DescriptorHeap* heaps[] = {srvHeap_.Get()};
    list_->SetDescriptorHeaps(1, heaps);
    list_->SetGraphicsRootDescriptorTable(1, srvHeap_->GetGPUDescriptorHandleForHeapStart());
    const ShaderParams sp = makeShaderParams(p, hasImage_, imageW_, imageH_);
    list_->SetGraphicsRoot32BitConstants(0, sizeof(ShaderParams) / 4, &sp, 0);
    list_->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list_->DrawInstanced(3, 1, 0, 0);
    b = transition(backBuffers_[bi].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    list_->ResourceBarrier(1, &b);
    if (FAILED(list_->Close())) return RenderResult::Fatal;

    ID3D12CommandList* lists[] = {list_.Get()};
    queue_->ExecuteCommandLists(1, lists);
    slotFence_[bi] = ++fenceValue_;
    queue_->Signal(fence_.Get(), slotFence_[bi]);
    return RenderResult::Rendered;
}

BackendInfo D3D12Backend::info() {
    BackendInfo b;
    fillInfo(b, "Direct3D 12");
    return b;
}

}  // namespace vrr
