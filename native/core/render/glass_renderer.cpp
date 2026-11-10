#include "glass_renderer.h"
#include "continuous_corners.h"

#include "compare_cs.h"
#include "glass_ps.h"
#include "glass_vs.h"
#include "backdrop_edge_ps.h"
#include "../motion.h"

#include <algorithm>
#include <cmath>

namespace atrium {

namespace {

constexpr UINT kCompareGroup = 16;

void Check(HRESULT hr) { winrt::check_hresult(hr); }

// Ease-out cubic from `from` to `to` over `t` seconds, starting at `begin`.
void EaseOut(IDCompositionAnimation* a, double begin, double t, float from, float to)
{
    float d = to - from;
    a->AddCubic(begin, from, float(3 * d / t), float(-3 * d / (t * t)), float(d / (t * t * t)));
}

// `spring` from `from` (at rest) to `to` as `segments` cubics over `length`
// seconds: each matches the spring's position and velocity at both of its
// ends (Hermite), so the compositor plays the spring itself, off our threads.
void SpringTo(IDCompositionAnimation* a, const motion::Spring& spring, float from, float to, double length, int segments)
{
    float x = from, v = 0;
    const float h = float(length / segments);
    for (int i = 0; i < segments; ++i) {
        float x1 = x, v1 = v;
        spring.Step(x1, v1, to, h);
        a->AddCubic(i * double(h), x, v, (3 * (x1 - x) / h - 2 * v - v1) / h, (2 * (x - x1) / h + v + v1) / (h * h));
        x = x1;
        v = v1;
    }
    a->End(length, to);
}

} // namespace

void PopVisual(IDCompositionDevice* device, IDCompositionVisual* visual, bool open, float cx, float cy)
{
    winrt::com_ptr<IDCompositionScaleTransform> scale;
    winrt::com_ptr<IDCompositionAnimation> s, o;
    winrt::com_ptr<IDCompositionEffectGroup> fx;
    if (FAILED(device->CreateScaleTransform(scale.put())) || FAILED(device->CreateAnimation(s.put())) ||
        FAILED(device->CreateAnimation(o.put())) || FAILED(device->CreateEffectGroup(fx.put())))
        return;
    if (open) {
        SpringTo(s.get(), motion::kPop, 0.92f, 1.0f, 0.6, 12); // a hint past full size, then rest
        EaseOut(o.get(), 0, 0.16, 0.0f, 1.0f);
        o->End(0.16, 1.0f);
    } else {
        EaseOut(s.get(), 0, 0.13, 1.0f, 0.95f);
        s->End(0.13, 0.95f);
        EaseOut(o.get(), 0, 0.12, 1.0f, 0.0f);
        o->End(0.12, 0.0f);
    }
    scale->SetCenterX(cx);
    scale->SetCenterY(cy);
    scale->SetScaleX(s.get());
    scale->SetScaleY(s.get());
    fx->SetOpacity(o.get());
    visual->SetTransform(scale.get());
    visual->SetEffect(fx.get());
    device->Commit();
}

void GlassRenderer::Pop(bool open, float cx, float cy) { PopVisual(dcomp_.get(), visual_.get(), open, cx, cy); }

GlassRenderer::GlassRenderer(const Gpu& gpu, HWND hwnd, int width, int height, bool topmost, bool overlay, bool alignedBackdrop)
    : gpu_(gpu), w_(width), h_(height), alignedBackdrop_(alignedBackdrop)
{
    sync_ = overlay ? 1 : 0; // the bar draws its content over the glass; a popup's content is its own
    auto* dev = gpu_.device.get();
    Check(dev->CreateVertexShader(g_GlassVS, sizeof(g_GlassVS), nullptr, vs_.put()));
    Check(dev->CreatePixelShader(g_GlassPS, sizeof(g_GlassPS), nullptr, ps_.put()));
    Check(dev->CreatePixelShader(g_BackdropEdgePS, sizeof(g_BackdropEdgePS), nullptr, edgePs_.put()));
    Check(dev->CreateComputeShader(g_CompareCS, sizeof(g_CompareCS), nullptr, cs_.put()));

    D3D11_BUFFER_DESC cbd{sizeof(GlassParams), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    Check(dev->CreateBuffer(&cbd, nullptr, cb_.put()));
    D3D11_BUFFER_DESC ccbd{16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER};
    Check(dev->CreateBuffer(&ccbd, nullptr, cmpCb_.put()));
    Check(dev->CreateBuffer(&ccbd, nullptr, edgeCb_.put()));
    // Out[0] changed pixels, Out[1] max difference (per-tile slots unused here).
    D3D11_BUFFER_DESC ob{4 * 4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 4};
    Check(dev->CreateBuffer(&ob, nullptr, cmpOut_.put()));
    D3D11_BUFFER_DESC sb{4 * 4, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ};
    for (auto& s : slots_)
        Check(dev->CreateBuffer(&sb, nullptr, s.counters.put()));
    D3D11_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_UNKNOWN;
    ud.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = 4;
    Check(dev->CreateUnorderedAccessView(cmpOut_.get(), &ud, cmpUav_.put()));

    D3D11_SAMPLER_DESC sd{};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    Check(dev->CreateSamplerState(&sd, sampler_.put()));
    // Premultiplied "over": several shapes (and their shadows) layer back to front.
    D3D11_BLEND_DESC bd{};
    auto& rt = bd.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlend = rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    Check(dev->CreateBlendState(&bd, blend_.put()));
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;
    Check(dev->CreateRasterizerState(&rd, scissor_.put()));

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = UINT(w_);
    desc.Height = UINT(h_);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    desc.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    Check(gpu_.factory->CreateSwapChainForComposition(dev, &desc, nullptr, swap_.put()));
    auto paced = swap_.as<IDXGISwapChain2>();
    Check(paced->SetMaximumFrameLatency(1));
    waitable_ = paced->GetFrameLatencyWaitableObject();

    auto dxgi = gpu_.device.as<IDXGIDevice>();
    Check(DCompositionCreateDevice(dxgi.get(), IID_PPV_ARGS(dcomp_.put())));
    Check(dcomp_->CreateTargetForHwnd(hwnd, topmost, target_.put()));
    Check(dcomp_->CreateVisual(visual_.put()));
    Check(visual_->SetContent(swap_.get()));
    Check(target_->SetRoot(visual_.get()));
    Check(dcomp_->Commit());

    if (overlay) {
        D2D1_FACTORY_OPTIONS fo{};
        Check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory3), &fo, d2dFactory_.put_void()));
        Check(d2dFactory_->CreateDevice(dxgi.get(), d2dDevice_.put()));
        Check(d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2d_.put()));
        Check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory3), reinterpret_cast<IUnknown**>(dwrite_.put())));
    }

    CreateSizeDependent();
}

GlassRenderer::~GlassRenderer()
{
    ForgetContinuousMasks(d2d_.get());
    if (waitable_)
        CloseHandle(waitable_);
}

bool GlassRenderer::Ready()
{
    if (!ready_)
        ready_ = WaitForSingleObject(waitable_, 0) == WAIT_OBJECT_0;
    return ready_;
}

void GlassRenderer::SetOffset(POINT offset)
{
    // DXGI applies this translation with the next pixel buffer, not as an
    // independently committed DirectComposition visual property.
    const DXGI_MATRIX_3X2_F matrix{1, 0, 0, 1, float(offset.x), float(offset.y)};
    Check(swap_.as<IDXGISwapChain2>()->SetMatrixTransform(&matrix));
}

void GlassRenderer::CreateSizeDependent()
{
    auto* dev = gpu_.device.get();
    // All blur levels (up to ceil(5.5) = 6) retain the desktop's texel phase.
    // A moving crop must not rebuild a different box-filter grid each frame.
    auto extent = [&](int size) { return alignedBackdrop_ ? UINT((size + 2 * kPad + 63 + 63) / 64 * 64) : UINT(size + 2 * kPad); };
    const UINT rw = roiW_ = extent(w_), rh = roiH_ = extent(h_);
    D3D11_TEXTURE2D_DESC td{};
    td.Width = rw;
    td.Height = rh;
    td.MipLevels = 0;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    td.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    roi_ = nullptr;
    roiSrv_ = nullptr;
    roiRtv_ = nullptr;
    edgeSourceSrv_ = nullptr;
    edgeSource_ = nullptr;
    Check(dev->CreateTexture2D(&td, nullptr, roi_.put()));
    Check(dev->CreateShaderResourceView(roi_.get(), nullptr, roiSrv_.put()));
    D3D11_RENDER_TARGET_VIEW_DESC rd{};
    rd.Format = td.Format;
    rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    Check(dev->CreateRenderTargetView(roi_.get(), &rd, roiRtv_.put()));
    // Luminance grid: the first mip at most 96 texels wide (~16 px cells).
    D3D11_TEXTURE2D_DESC created{};
    roi_->GetDesc(&created);
    gridMip_ = 0;
    while (gridMip_ + 1 < created.MipLevels && (rw >> gridMip_) > 96)
        ++gridMip_;
    gridW_ = std::max(1, int(rw >> gridMip_));
    gridH_ = std::max(1, int(rh >> gridMip_));
    D3D11_TEXTURE2D_DESC gd{UINT(gridW_), UINT(gridH_), 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, {1, 0}, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0};
    for (auto& s : slots_) {
        s.grid = nullptr;
        Check(dev->CreateTexture2D(&gd, nullptr, s.grid.put()));
    }

    td.MipLevels = 1;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.MiscFlags = 0;
    presented_ = nullptr;
    presentedSrv_ = nullptr;
    Check(dev->CreateTexture2D(&td, nullptr, presented_.put()));
    Check(dev->CreateShaderResourceView(presented_.get(), nullptr, presentedSrv_.put()));

    const UINT cp[4] = {rw, rh, 1, 1u << 30}; // one tile covering everything
    gpu_.context->UpdateSubresource(cmpCb_.get(), 0, nullptr, cp, 0, 0);
    hasPresented_ = false;
    hasBackdrop_ = false;
    comparisonPending_ = false;
    mipsDirty_ = false;
    inFlight_.clear(); // results for the old size are meaningless
}

void GlassRenderer::Resize(int width, int height)
{
    if (width == w_ && height == h_)
        return;
    w_ = width;
    h_ = height;
    if (d2d_)
        d2d_->SetTarget(nullptr);
    rtv_ = nullptr;
    overlayTarget_ = nullptr;
    Check(swap_->ResizeBuffers(2, UINT(w_), UINT(h_), DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT));
    CreateSizeDependent();
}

void GlassRenderer::UpdateBackdrop(ID3D11Texture2D* frame, int x, int y)
{
    D3D11_TEXTURE2D_DESC fd{};
    frame->GetDesc(&fd);
    int x0 = x - kPad, y0 = y - kPad;
    if (alignedBackdrop_) {
        x0 = int(std::floor(double(x0) / 64)) * 64;
        y0 = int(std::floor(double(y0) / 64)) * 64;
    }
    roiOrigin_ = POINT{x - x0, y - y0};
    int x1 = x0 + int(roiW_), y1 = y0 + int(roiH_);
    int cx0 = std::max(0, x0), cy0 = std::max(0, y0), cx1 = std::min(int(fd.Width), x1), cy1 = std::min(int(fd.Height), y1);
    if (!fd.Width || !fd.Height) return;
    if (cx1 > cx0 && cy1 > cy0) {
        D3D11_BOX box{UINT(cx0), UINT(cy0), 0, UINT(cx1), UINT(cy1), 1};
        gpu_.context->CopySubresourceRegion(roi_.get(), 0, UINT(cx0 - x0), UINT(cy0 - y0), 0, frame, 0, &box);
    }
    if (x0 < 0 || y0 < 0 || x1 > int(fd.Width) || y1 > int(fd.Height))
        ExtendBackdropEdges(frame, fd, x0, y0);
    mipsDirty_ = true;
    hasBackdrop_ = true;
    comparisonPending_ = true;
    QueueComparison();
}

void GlassRenderer::ExtendBackdropEdges(ID3D11Texture2D* frame, const D3D11_TEXTURE2D_DESC& source, int x, int y)
{
    auto* ctx = gpu_.context.get();
    if (!edgeSource_) {
        // Reused local scratch, allocated only at a monitor edge. Captured
        // textures need not have SHADER_RESOURCE binding, so copy their borders.
        D3D11_TEXTURE2D_DESC td{roiW_, roiH_, 1, 1, source.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE};
        Check(gpu_.device->CreateTexture2D(&td, nullptr, edgeSource_.put()));
        Check(gpu_.device->CreateShaderResourceView(edgeSource_.get(), nullptr, edgeSourceSrv_.put()));
    }
    const int l = std::clamp(x, 0, int(source.Width) - 1), t = std::clamp(y, 0, int(source.Height) - 1);
    const int r = std::clamp(x + int(roiW_), 1, int(source.Width)), b = std::clamp(y + int(roiH_), 1, int(source.Height));
    const int width = r - l, height = b - t;
    auto copy = [&](UINT dx, UINT dy, int left, int top, int right, int bottom) {
        D3D11_BOX box{UINT(left), UINT(top), 0, UINT(right), UINT(bottom), 1};
        ctx->CopySubresourceRegion(edgeSource_.get(), 0, dx, dy, 0, frame, 0, &box);
    };
    if (y < 0) copy(0, 0, l, t, r, t + 1);
    if (y + int(roiH_) > int(source.Height)) copy(0, UINT(height - 1), l, b - 1, r, b);
    if (x < 0) copy(0, 0, l, t, l + 1, b);
    if (x + int(roiW_) > int(source.Width)) copy(UINT(width - 1), 0, r - 1, t, r, b);

    const int params[] = {x - l, y - t, width, height};
    ctx->UpdateSubresource(edgeCb_.get(), 0, nullptr, params, 0, 0);
    ID3D11Buffer* cb = edgeCb_.get();
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ID3D11RenderTargetView* target = roiRtv_.get();
    ctx->OMSetRenderTargets(1, &target, nullptr);
    ctx->OMSetBlendState(nullptr, nullptr, ~0u);
    D3D11_VIEWPORT viewport{0, 0, float(roiW_), float(roiH_), 0, 1};
    ctx->RSSetViewports(1, &viewport);
    ctx->RSSetState(scissor_.get());
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vs_.get(), nullptr, 0);
    ctx->PSSetShader(edgePs_.get(), nullptr, 0);
    ID3D11ShaderResourceView* resource = edgeSourceSrv_.get();
    ctx->PSSetShaderResources(0, 1, &resource);
    auto draw = [&](int left, int top, int right, int bottom) {
        if (right <= left || bottom <= top) return;
        D3D11_RECT rect{left, top, right, bottom};
        ctx->RSSetScissorRects(1, &rect);
        ctx->Draw(3, 0);
    };
    const int top = std::clamp(-y, 0, int(roiH_)), bottom = std::clamp(int(source.Height) - y, 0, int(roiH_));
    draw(0, 0, int(roiW_), top);
    draw(0, bottom, int(roiW_), int(roiH_));
    draw(0, top, std::clamp(-x, 0, int(roiW_)), bottom);
    draw(std::clamp(int(source.Width) - x, 0, int(roiW_)), top, int(roiW_), bottom);
    resource = nullptr;
    ctx->PSSetShaderResources(0, 1, &resource);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);
    ctx->RSSetState(nullptr);
}

void GlassRenderer::PrepareBackdrop()
{
    if (!mipsDirty_)
        return;
    gpu_.context->GenerateMips(roiSrv_.get());
    mipsDirty_ = false;
}

bool GlassRenderer::QueueComparison()
{
    // A busy GPU must never make the render thread wait for a readback. Keep
    // the newest ROI and submit its comparison once Poll retires a slot.
    if (!comparisonPending_ || !hasPresented_ || int(inFlight_.size()) == kSlots)
        return false;
    comparisonPending_ = false;
    PrepareBackdrop(); // coalesce mip generation too while the readback ring is full
    auto* ctx = gpu_.context.get();
    const UINT zero[4] = {};
    ctx->ClearUnorderedAccessViewUint(cmpUav_.get(), zero);
    ctx->CSSetShader(cs_.get(), nullptr, 0);
    ID3D11ShaderResourceView* srvs[] = {roiSrv_.get(), presentedSrv_.get()};
    ctx->CSSetShaderResources(0, 2, srvs);
    ID3D11UnorderedAccessView* uav = cmpUav_.get();
    ctx->CSSetUnorderedAccessViews(0, 1, &uav, nullptr);
    ID3D11Buffer* cb = cmpCb_.get();
    ctx->CSSetConstantBuffers(0, 1, &cb);
    ctx->Dispatch((roiW_ + kCompareGroup - 1) / kCompareGroup, (roiH_ + kCompareGroup - 1) / kCompareGroup, 1);
    ID3D11ShaderResourceView* noSrv[2] = {};
    ctx->CSSetShaderResources(0, 2, noSrv);
    ID3D11UnorderedAccessView* noUav = nullptr;
    ctx->CSSetUnorderedAccessViews(0, 1, &noUav, nullptr);
    int slot = nextSlot_;
    slots_[slot].origin = roiOrigin_;
    nextSlot_ = (nextSlot_ + 1) % kSlots;
    ctx->CopyResource(slots_[slot].counters.get(), cmpOut_.get());
    ctx->CopySubresourceRegion(slots_[slot].grid.get(), 0, 0, 0, 0, roi_.get(), gridMip_, nullptr);
    inFlight_.push_back(slot); // the render loop flushes once per wake-up; the result is polled later
    return true;
}

bool GlassRenderer::ReadSlot(int slot, Readback& out)
{
    auto* ctx = gpu_.context.get();
    D3D11_MAPPED_SUBRESOURCE m{};
    HRESULT hr = ctx->Map(slots_[slot].counters.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        return false;
    Check(hr); // a lost device is rebuilt by RenderThread, not polled indefinitely
    out.changed = static_cast<const UINT*>(m.pData)[0] != 0;
    ctx->Unmap(slots_[slot].counters.get(), 0);
    hr = ctx->Map(slots_[slot].grid.get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &m);
    if (hr == DXGI_ERROR_WAS_STILL_DRAWING)
        return false; // the counter copy can finish before the luminance copy
    Check(hr);
    out.gridW = gridW_;
    out.gridH = gridH_;
    out.cellX = float(roiW_) / float(gridW_);
    out.cellY = float(roiH_) / float(gridH_);
    out.origin = slots_[slot].origin;
    out.luma.resize(size_t(gridW_) * size_t(gridH_));
    for (int y = 0; y < gridH_; ++y) {
        const auto* row = static_cast<const uint8_t*>(m.pData) + size_t(y) * m.RowPitch;
        for (int x = 0; x < gridW_; ++x) {
            const uint8_t* px = row + 4 * x; // BGRA
            out.luma[size_t(y) * gridW_ + x] = (0.0722f * px[0] + 0.7152f * px[1] + 0.2126f * px[2]) / 255.f;
        }
    }
    ctx->Unmap(slots_[slot].grid.get(), 0);
    return true;
}

float Readback::RegionLuma(float x0, float y0, float x1, float y1) const
{
    if (luma.empty())
        return 0.5f;
    int gx0 = std::clamp(int(x0 / cellX), 0, gridW - 1), gx1 = std::clamp(int(x1 / cellX), 0, gridW - 1);
    int gy0 = std::clamp(int(y0 / cellY), 0, gridH - 1), gy1 = std::clamp(int(y1 / cellY), 0, gridH - 1);
    float sum = 0;
    int n = 0;
    for (int y = gy0; y <= gy1; ++y)
        for (int x = gx0; x <= gx1; ++x, ++n)
            sum += luma[size_t(y) * gridW + x];
    return n ? sum / float(n) : 0.5f;
}

std::optional<Readback> GlassRenderer::Poll()
{
    std::optional<Readback> merged;
    while (!inFlight_.empty()) {
        Readback r;
        if (!ReadSlot(inFlight_.front(), r))
            break;
        inFlight_.pop_front();
        bool changed = r.changed || (merged && merged->changed);
        merged = r;
        merged->changed = changed;
    }
    // Poll can run after the host's batch Flush. Submit deferred work here so
    // it also completes when the desktop stops producing capture frames.
    if (QueueComparison())
        gpu_.context->Flush();
    return merged;
}

bool GlassRenderer::Present(const GlassParams& params, const std::function<void(ID2D1DeviceContext2*)>& overlay)
{
    return Present(std::vector<GlassParams>{params}, overlay);
}

bool GlassRenderer::Present(const std::vector<GlassParams>& shapes, const std::function<void(ID2D1DeviceContext2*)>& overlay)
{
    PrepareBackdrop(); // a present can need the newest glass before its comparison completes
    auto* ctx = gpu_.context.get();
    if (!rtv_) {
        winrt::com_ptr<ID3D11Texture2D> back;
        Check(swap_->GetBuffer(0, IID_PPV_ARGS(back.put())));
        Check(gpu_.device->CreateRenderTargetView(back.get(), nullptr, rtv_.put()));
    }
    ID3D11RenderTargetView* rt = rtv_.get();
    const float clear[4] = {0, 0, 0, 0};
    ctx->ClearRenderTargetView(rt, clear);
    ctx->OMSetRenderTargets(1, &rt, nullptr);
    ctx->OMSetBlendState(blend_.get(), nullptr, 0xFFFFFFFF);
    D3D11_VIEWPORT vp{0, 0, float(w_), float(h_), 0, 1};
    ctx->RSSetViewports(1, &vp);
    ctx->RSSetState(scissor_.get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetInputLayout(nullptr);
    ctx->VSSetShader(vs_.get(), nullptr, 0);
    ctx->PSSetShader(ps_.get(), nullptr, 0);
    ID3D11Buffer* cb = cb_.get();
    ctx->PSSetConstantBuffers(0, 1, &cb);
    ID3D11ShaderResourceView* srv = roiSrv_.get();
    ctx->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* s = sampler_.get();
    ctx->PSSetSamplers(0, 1, &s);
    for (size_t k = 0; k < shapes.size(); ++k) {
        GlassParams p = shapes[k];
        p.sizeX = float(w_);
        p.sizeY = float(h_);
        p.roiOriginX = float(roiOrigin_.x);
        p.roiOriginY = float(roiOrigin_.y);
        p.roiSizeX = float(roiW_);
        p.roiSizeY = float(roiH_);
        if (p.shapeR <= p.shapeL || p.shapeB <= p.shapeT) {
            p.shapeL = p.shapeT = p.inset;
            p.shapeR = float(w_) - p.inset;
            p.shapeB = float(h_) - p.inset;
        }
        // The shader runs only over the shape's bounds and its shadow (a tall
        // popup with a few cards would otherwise shade every pixel per card).
        float l = p.shapeL, t = p.shapeT - p.waveHeight, r = p.shapeR, b = p.shapeB;
        if (p.tailHeight > 0)
            b += p.tailHeight;
        for (int i = 0; i < int(p.partCount) && i < kMaxGlassParts; ++i) {
            l = std::min(l, p.parts[i][0]);
            t = std::min(t, p.parts[i][1]);
            r = std::max(r, p.parts[i][2]);
            b = std::max(b, p.parts[i][3]);
        }
        const float m = 2 * std::max(p.inset, 1.f) + 2; // the shadow has faded out by then
        D3D11_RECT sc{LONG(std::max(0.f, std::floor(l - m))), LONG(std::max(0.f, std::floor(t - m))), LONG(std::min(float(w_), std::ceil(r + m))),
                      LONG(std::min(float(h_), std::ceil(b + m)))};
        if (sc.right <= sc.left || sc.bottom <= sc.top)
            continue;
        ctx->RSSetScissorRects(1, &sc);
        ctx->UpdateSubresource(cb_.get(), 0, nullptr, &p, 0, 0);
        ctx->Draw(3, 0);
    }
    ctx->RSSetState(nullptr);
    ID3D11ShaderResourceView* none[2] = {};
    ctx->PSSetShaderResources(0, 2, none);
    ctx->OMSetRenderTargets(0, nullptr, nullptr);

    if (overlay && d2d_) {
        if (!overlayTarget_) {
            winrt::com_ptr<IDXGISurface> surface;
            Check(swap_->GetBuffer(0, IID_PPV_ARGS(surface.put())));
            D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                                 D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            Check(d2d_->CreateBitmapFromDxgiSurface(surface.get(), &bp, overlayTarget_.put()));
        }
        d2d_->SetTarget(overlayTarget_.get());
        d2d_->BeginDraw();
        d2d_->SetTransform(D2D1::Matrix3x2F::Identity());
        overlay(d2d_.get());
        HRESULT hr = d2d_->EndDraw();
        d2d_->SetTarget(nullptr);
        if (hr == D2DERR_RECREATE_TARGET)
            return false;
    }

    ctx->CopySubresourceRegion(presented_.get(), 0, 0, 0, 0, roi_.get(), 0, nullptr);
    hasPresented_ = hasBackdrop_;
    HRESULT hr = swap_->Present(sync_, 0);
    ready_ = false;
    return hr != DXGI_ERROR_DEVICE_REMOVED && hr != DXGI_ERROR_DEVICE_RESET;
}

} // namespace atrium
