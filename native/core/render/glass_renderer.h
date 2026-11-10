// Renders the glass material for one surface: copies a padded backdrop ROI from
// the capture, builds mips for the blur, draws one full-window triangle and the
// text overlay into a premultiplied composition swap chain hosted by
// DirectComposition. Exact echo detection compares the ROI with the ROI that
// was last presented.
#pragma once

#include "gpu.h"

#include <d2d1_3.h>
#include <dcomp.h>
#include <dwrite_3.h>

#include <deque>
#include <functional>
#include <optional>
#include <vector>

namespace atrium {

// Mirrors cbuffer GlassParams in shaders/glass.hlsl (368 bytes). Lengths in px.
struct GlassParams {
    float sizeX, sizeY, roiOriginX, roiOriginY;
    float roiSizeX, roiSizeY, radius, edge;
    float refract, dispersion, blurLod, tintMix;
    float tintR, tintG, tintB, tintA;
    float highlight, shadow, lightX, lightY;
    float inset, blurMix, fade, pad0; // fade: 0 = as is, 1 = gone
    // The shape in window px; all zero: the window minus `inset` on every side.
    float shapeL, shapeT, shapeR, shapeB;
    // Press ripple: centre (window px), ring radius, strength; strength 0 = none.
    float rippleX, rippleY, rippleR, rippleA;
    // Melting parts (GlassShape::parts), window px; partCount 0: the shape above.
    float parts[6][4];
    float partRadii[6][4];
    float partCount, melt, partMix, pad1; // partMix: 0 the parts, 1 the rectangle above (Shape, Radius)
    float tailX, tailHalfWidth, tailHeight, tailInset; // optional Dock label pointer
    float waveX, waveWidth, waveHeight, wavePad; // optional upward Dock hover wave
};
static_assert(sizeof(GlassParams) == 368);
constexpr int kMaxGlassParts = 6;

// Scale and fade of a composition visual, run by DWM so it stays smooth
// whatever our own frame rate is. Opening: from slightly small and clear,
// overshooting a hair before settling; closing: shrinks a little and fades.
// (cx, cy): the fixed point of the scale, in the visual's px.
void PopVisual(IDCompositionDevice* device, IDCompositionVisual* visual, bool open, float cx, float cy);

// Result of one backdrop comparison, read back without stalling the CPU.
struct Readback {
    bool changed = false; // ROI differed from the last presented ROI
    POINT origin{64, 64}; // content origin in the mip texture that produced this result
    // Backdrop luminance (0..1) on a coarse grid over the ROI; one cell covers
    // cellX x cellY ROI pixels. The ROI is the window plus GlassRenderer::kPad.
    std::vector<float> luma;
    int gridW = 0, gridH = 0;
    float cellX = 1, cellY = 1;
    // Mean luminance over a rectangle in ROI pixels.
    float RegionLuma(float x0, float y0, float x1, float y1) const;
};

class GlassRenderer {
public:
    static constexpr int kPad = 64; // backdrop margin for refraction and blur

    // topmost: the composition target's layer. A popup window has two: the
    // glass below (topmost = false) and its content drawn by Surface above.
    // overlay: whether Present draws a Direct2D overlay (the popups' glass does not).
    GlassRenderer(const Gpu& gpu, HWND hwnd, int width, int height, bool topmost = true, bool overlay = true, bool alignedBackdrop = false);
    ~GlassRenderer();
    GlassRenderer(const GlassRenderer&) = delete;
    GlassRenderer& operator=(const GlassRenderer&) = delete;

    void Resize(int width, int height);
    void SetOffset(POINT offset);
    int Width() const { return w_; }
    int Height() const { return h_; }

    // Copies the backdrop under the surface (monitor-relative px) into the ROI
    // and queues its comparison with the ROI used for the last present (D04).
    void UpdateBackdrop(ID3D11Texture2D* monitorFrame, int x, int y);
    bool HasPresented() const { return hasPresented_; }
    bool HasBackdrop() const { return hasBackdrop_; }
    // Runs PopVisual on this surface's visual.
    void Pop(bool open, float cx, float cy);
    // Completed comparisons since the last call, merged (changed = any changed,
    // colour = newest), or nullopt if none finished yet. Never waits.
    std::optional<Readback> Poll();
    bool ReadbackPending() const { return (comparisonPending_ && hasPresented_) || !inFlight_.empty(); }

    // Draws the glass, then `overlay` with D2D on the same back buffer, and
    // presents. Returns false if the device was lost.
    bool Present(const GlassParams& params, const std::function<void(ID2D1DeviceContext2*)>& overlay);
    // Several shapes, back to front (each with its own shadow), then the overlay.
    bool Present(const std::vector<GlassParams>& shapes, const std::function<void(ID2D1DeviceContext2*)>& overlay);

    // D45: the swap chain queues one frame at most, and its latency object is signalled when
    // that frame has been taken for the screen. Ready() takes the signal without waiting (once
    // taken it holds until Present); the render thread draws a surface only when it is ready,
    // so it never waits for one surface while others have frames to draw.
    bool Ready();
    ID2D1DeviceContext2* D2D() const { return d2d_.get(); }
    IDWriteFactory3* DWrite() const { return dwrite_.get(); }

private:
    void CreateSizeDependent();
    void PrepareBackdrop();
    void ExtendBackdropEdges(ID3D11Texture2D* frame, const D3D11_TEXTURE2D_DESC& source, int x, int y);

    Gpu gpu_;
    int w_, h_;
    bool alignedBackdrop_ = false;
    UINT roiW_ = 0, roiH_ = 0;
    POINT roiOrigin_{kPad, kPad};
    winrt::com_ptr<IDXGISwapChain1> swap_;
    // Views of the back buffer (buffer 0 is always the one to draw in): made
    // once, dropped before the buffers are resized.
    winrt::com_ptr<ID3D11RenderTargetView> rtv_;
    winrt::com_ptr<ID2D1Bitmap1> overlayTarget_;
    winrt::com_ptr<IDCompositionDevice> dcomp_;
    winrt::com_ptr<IDCompositionTarget> target_;
    winrt::com_ptr<IDCompositionVisual> visual_;
    winrt::com_ptr<ID3D11VertexShader> vs_;
    winrt::com_ptr<ID3D11PixelShader> ps_;
    winrt::com_ptr<ID3D11PixelShader> edgePs_;
    winrt::com_ptr<ID3D11Buffer> edgeCb_;
    winrt::com_ptr<ID3D11RenderTargetView> roiRtv_;
    winrt::com_ptr<ID3D11Texture2D> edgeSource_;
    winrt::com_ptr<ID3D11ShaderResourceView> edgeSourceSrv_;
    winrt::com_ptr<ID3D11ComputeShader> cs_;
    // Readback ring: comparison counters and a small mip (luminance grid).
    struct Slot {
        winrt::com_ptr<ID3D11Buffer> counters;
        winrt::com_ptr<ID3D11Texture2D> grid;
        POINT origin{};
    };
    static constexpr int kSlots = 3;
    Slot slots_[kSlots];
    std::deque<int> inFlight_;
    int nextSlot_ = 0;
    UINT gridMip_ = 0;
    int gridW_ = 1, gridH_ = 1;
    bool comparisonPending_ = false; // the newest ROI, deferred while all slots are in flight
    bool mipsDirty_ = false;
    bool QueueComparison();
    bool ReadSlot(int slot, Readback& out);

    winrt::com_ptr<ID3D11Buffer> cb_, cmpCb_, cmpOut_;
    winrt::com_ptr<ID3D11UnorderedAccessView> cmpUav_;
    winrt::com_ptr<ID3D11SamplerState> sampler_;
    winrt::com_ptr<ID3D11BlendState> blend_;
    winrt::com_ptr<ID3D11RasterizerState> scissor_; // each shape only where it and its shadow are
    winrt::com_ptr<ID3D11Texture2D> roi_, presented_;
    winrt::com_ptr<ID3D11ShaderResourceView> roiSrv_, presentedSrv_;
    bool hasPresented_ = false;
    // The bar waits for the display between frames; a popup's glass is shown
    // at once, as its content is (Surface presents without waiting), so a
    // frame of the two lands in the same composition.
    UINT sync_ = 1;
    bool hasBackdrop_ = false;
    HANDLE waitable_ = nullptr;
    bool ready_ = false;

    winrt::com_ptr<ID2D1Factory3> d2dFactory_;
    winrt::com_ptr<ID2D1Device2> d2dDevice_;
    winrt::com_ptr<ID2D1DeviceContext2> d2d_;
    winrt::com_ptr<IDWriteFactory3> dwrite_;
};

} // namespace atrium
