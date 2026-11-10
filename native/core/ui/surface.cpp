#include "ui/surface.h"

#include "clock.h"
#include "render/capture.h"
#include "render/continuous_corners.h"
#include "render/callout_profile.h"
#include "log.h"
#include "render/glass_renderer.h"
#include "render/round_stroke.h"
#include "render/ui_font.h"
#include "render/wifi_glyph.h"

#include <d2d1effects.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dxguid.lib") // CLSID_D2D1GaussianBlur

namespace atrium {

float PopupScale(RECT bar)
{
    HMONITOR mon = MonitorFromRect(&bar, MONITOR_DEFAULTTOPRIMARY);
    UINT dpi = 96, dpiY = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dpiY);
    return float(dpi) / 96.f;
}

RECT PopupRect(RECT bar, D2D1_SIZE_F sizeDip, PopupAlign align)
{
    HMONITOR mon = MonitorFromRect(&bar, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const RECT& wa = mi.rcWork;
    float s = PopupScale(bar);
    int w = int(std::lround(sizeDip.width * s)), h = int(std::lround(sizeDip.height * s)), gap = int(std::lround(kPopupGapDip * s));
    int bottom = int(bar.top) - gap;
    int left = align == PopupAlign::Left ? int(bar.left) : int(bar.right) - w;
    left = std::max(int(wa.left) + gap, std::min(left, int(wa.right) - gap - w));
    return RECT{left, std::max(int(wa.top) + gap, bottom - h), left + w, bottom};
}

namespace {

constexpr wchar_t kClass[] = L"AtriumSurface";
constexpr UINT_PTR kHideTimer = 0x5A01, kGlassWaitTimer = 0x5A03;
constexpr UINT kHideMs = 140;     // the closing pop (PopVisual) lasts 130 ms
constexpr double kGlassWaitMs = 300; // then a plain panel stands in for missing glass

constexpr UINT WM_SURFACE_FRAME = WM_APP + 0x42;

D2D1_COLOR_F Rgba(uint32_t rgb, float a) { return D2D1::ColorF(rgb, a); }

// Frames for the popups' animations, in step with the compositor: a thread
// waits for each composition (DwmFlush) and tells the windows that want a
// frame. (A 10 ms timer really fires every 16-31 ms and stutters.)
class FramePump {
public:
    static FramePump& Get()
    {
        static FramePump pump;
        return pump;
    }
    void Want(HWND h)
    {
        std::lock_guard g(mutex_);
        if (std::find(wanting_.begin(), wanting_.end(), h) == wanting_.end())
            wanting_.push_back(h);
        if (!thread_.joinable())
            thread_ = std::thread([this] { Run(); });
        cv_.notify_one();
    }

private:
    ~FramePump()
    {
        {
            std::lock_guard g(mutex_);
            stop_ = true;
        }
        cv_.notify_one();
        if (thread_.joinable())
            thread_.join();
    }
    void Run()
    {
        std::vector<HWND> due;
        for (;;) {
            {
                std::unique_lock g(mutex_);
                cv_.wait(g, [&] { return stop_ || !wanting_.empty(); });
                if (stop_)
                    return;
            }
            if (FAILED(DwmFlush()))
                Sleep(8);
            {
                std::lock_guard g(mutex_);
                due.swap(wanting_);
            }
            for (HWND h : due)
                PostMessageW(h, WM_SURFACE_FRAME, 0, 0);
            due.clear();
        }
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<HWND> wanting_;
    bool stop_ = false;
    std::thread thread_;
};

uint32_t g_tint = 0xFFFFFF;
double g_transparency = 0.95;


} // namespace

GlassHost* Surface::host_ = nullptr;
bool Surface::capturable_ = false;
bool Surface::captureVisible_ = false;
std::vector<Surface*> Surface::all_;

void Theme::SetGlass(uint32_t tint, double transparency)
{
    g_tint = tint;
    g_transparency = transparency;
}

Theme Theme::For(bool darkInk)
{
    // The bar's inks and washes (BarPainter::DrawContent).
    Theme t;
    t.light = darkInk;
    PanelTone tone = PanelToneFor(g_tint, g_transparency);
    t.panel = Rgba(tone.tint, 0.96f);
    if (darkInk) {
        t.border = Rgba(0x000000, 0.10f);
        t.text = Rgba(0x111317, 0.92f);
        t.subtext = Rgba(0x111317, 0.66f);
        t.faint = Rgba(0x111317, 0.46f);
        t.hover = Rgba(0x000000, 0.10f);
        t.press = Rgba(0x000000, 0.16f);
        t.divider = Rgba(0x000000, 0.12f);
        t.card = Rgba(0x000000, 0.05f);
        t.cardHot = Rgba(0x000000, 0.10f);
        t.halo = Rgba(0xFFFFFF, 0.35f);
    } else {
        t.border = Rgba(0xFFFFFF, 0.30f);
        t.text = Rgba(0xFFFFFF, 0.96f);
        t.subtext = Rgba(0xFFFFFF, 0.76f);
        t.faint = Rgba(0xFFFFFF, 0.56f);
        t.hover = Rgba(0xFFFFFF, 0.20f);
        t.press = Rgba(0xFFFFFF, 0.28f);
        t.divider = Rgba(0xFFFFFF, 0.24f);
        t.card = Rgba(0xFFFFFF, 0.08f);
        t.cardHot = Rgba(0xFFFFFF, 0.20f);
        t.halo = Rgba(0x000000, 0.32f);
    }
    t.accent = Rgba(0x0A84FF, 1);
    t.accentText = Rgba(0xFFFFFF, 1);
    return t;
}

Theme Theme::Mix(float darkInk)
{
    if (darkInk <= 0.001f || darkInk >= 0.999f)
        return For(darkInk >= 0.5f);
    Theme a = For(false), b = For(true), t = b;
    auto mix = [&](D2D1_COLOR_F Theme::*c) {
        const D2D1_COLOR_F &x = a.*c, &y = b.*c;
        t.*c = D2D1::ColorF(x.r + (y.r - x.r) * darkInk, x.g + (y.g - x.g) * darkInk, x.b + (y.b - x.b) * darkInk, x.a + (y.a - x.a) * darkInk);
    };
    for (auto c : {&Theme::border, &Theme::text, &Theme::subtext, &Theme::faint, &Theme::hover, &Theme::press, &Theme::divider, &Theme::card,
                   &Theme::cardHot, &Theme::halo})
        mix(c);
    t.light = darkInk >= 0.5f;
    return t;
}

Theme Theme::Panel()
{
    Theme t = For(PanelToneFor(g_tint, g_transparency).light);
    t.halo = D2D1::ColorF(0, 0, 0, 0); // an opaque panel needs none
    return t;
}

// ---- Canvas ------------------------------------------------------------------

D2D1_RECT_F Canvas::Px(D2D1_RECT_F r) const { return D2D1::RectF(r.left * scale, r.top * scale, r.right * scale, r.bottom * scale); }

ID2D1SolidColorBrush* Canvas::Brush(D2D1_COLOR_F c)
{
    c.a *= alpha_; // the fade of the enclosing groups
    if (!s_.brush_)
        d2d_->CreateSolidColorBrush(c, s_.brush_.put());
    else
        s_.brush_->SetColor(c);
    return s_.brush_.get();
}

void Canvas::Fill(D2D1_RECT_F r, float radius, D2D1_COLOR_F c)
{
    D2D1_COLOR_F unused;
    c = InkAt(r, c, unused);
    if (radius > 0)
        FillContinuousRect(d2d_, Px(r), radius * scale, Brush(c));
    else
        d2d_->FillRectangle(Px(r), Brush(c));
}

void Canvas::Callout(D2D1_RECT_F body, float radius, float tipX, float halfWidth, float tailHeight, float tailInset, D2D1_COLOR_F color)
{
    if (tailHeight <= 0) { Fill(body, radius, color); return; }
    auto rect = Px(body);
    rect = D2D1::RectF(std::round(rect.left), std::round(rect.top), std::round(rect.right), std::round(rect.bottom));
    const float width = rect.right - rect.left, height = rect.bottom - rect.top;
    if (width <= 0 || height <= 0) return;
    const std::array<float, 7> key{width, height, radius * scale, std::round(tipX * scale) - rect.left,
                                  halfWidth * scale, tailHeight * scale, tailInset * scale};
    if (!s_.calloutMask_ || key != s_.calloutMaskKey_) {
        const UINT32 w = UINT32(std::ceil(width)), h = UINT32(std::ceil(height + key[5]));
        std::vector<BYTE> pixels(size_t(w) * h);
        for (UINT32 y = 0; y < h; ++y)
            for (UINT32 x = 0; x < w; ++x)
                pixels[size_t(y) * w + x] = BYTE(std::lround(255 * std::clamp(0.5f - CalloutEdge(float(x) + 0.5f, float(y) + 0.5f,
                    key[0], key[1], key[2], key[3], key[4], key[5], key[6]), 0.f, 1.f)));
        s_.calloutMask_ = nullptr;
        const auto properties = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
        if (FAILED(d2d_->CreateBitmap(D2D1::SizeU(w, h), pixels.data(), w, properties, s_.calloutMask_.put()))) return;
        s_.calloutMaskKey_ = key;
    }
    const auto antialias = d2d_->GetAntialiasMode();
    d2d_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
    const auto size = s_.calloutMask_->GetSize();
    d2d_->FillOpacityMask(s_.calloutMask_.get(), Brush(color), D2D1_OPACITY_MASK_CONTENT_GRAPHICS,
        D2D1::RectF(rect.left, rect.top, rect.left + size.width, rect.top + size.height), D2D1::RectF(0, 0, size.width, size.height));
    d2d_->SetAntialiasMode(antialias);
}

void Canvas::Stroke(D2D1_RECT_F r, float radius, D2D1_COLOR_F c, float width)
{
    D2D1_COLOR_F unused;
    c = InkAt(r, c, unused);
    float h = width * scale / 2;
    D2D1_RECT_F p = Px(r);
    p = D2D1::RectF(p.left + h, p.top + h, p.right - h, p.bottom - h);
    StrokeContinuousRect(d2d_, p, std::max(0.f, radius * scale - h), Brush(c), width * scale);
}

void Canvas::Line(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width)
{
    D2D1_COLOR_F unused;
    c = InkAt(D2D1::RectF(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1) + 1, std::max(y0, y1) + 1), c, unused);
    d2d_->DrawLine(D2D1::Point2F(x0 * scale, y0 * scale), D2D1::Point2F(x1 * scale, y1 * scale), Brush(c), width * scale);
}

void Canvas::Capsule(float x0, float y0, float x1, float y1, D2D1_COLOR_F c, float width)
{
    D2D1_COLOR_F unused;
    c = InkAt(D2D1::RectF(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1) + 1, std::max(y0, y1) + 1), c, unused);
    d2d_->DrawLine(D2D1::Point2F(x0 * scale, y0 * scale), D2D1::Point2F(x1 * scale, y1 * scale), Brush(c), width * scale, RoundStroke(d2d_));
}

void Canvas::Circle(float cx, float cy, float radius, D2D1_COLOR_F c)
{
    D2D1_COLOR_F unused;
    c = InkAt(D2D1::RectF(cx - radius, cy - radius, cx + radius, cy + radius), c, unused);
    d2d_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx * scale, cy * scale), radius * scale, radius * scale), Brush(c));
}

void Canvas::Gradient(D2D1_RECT_F r, float radius, D2D1_COLOR_F a, D2D1_COLOR_F b, bool vertical)
{
    a.a *= alpha_;
    b.a *= alpha_;
    const D2D1_RECT_F p = Px(r);
    D2D1_GRADIENT_STOP stops[] = {{0, a}, {1, b}};
    winrt::com_ptr<ID2D1GradientStopCollection> collection;
    winrt::com_ptr<ID2D1LinearGradientBrush> brush;
    if (FAILED(d2d_->CreateGradientStopCollection(stops, 2, collection.put())) ||
        FAILED(d2d_->CreateLinearGradientBrush(
            D2D1::LinearGradientBrushProperties(D2D1::Point2F(p.left, p.top), vertical ? D2D1::Point2F(p.left, p.bottom) : D2D1::Point2F(p.right, p.top)),
            collection.get(), brush.put())))
        return;
    if (radius > 0)
        FillContinuousRect(d2d_, p, radius * scale, brush.get());
    else
        d2d_->FillRectangle(p, brush.get());
}

void Canvas::Crescent(float cx, float cy, float radius, float dx, float dy, float cut, D2D1_COLOR_F c)
{
    winrt::com_ptr<ID2D1Factory> f;
    d2d_->GetFactory(f.put());
    winrt::com_ptr<ID2D1EllipseGeometry> disc, bite;
    winrt::com_ptr<ID2D1PathGeometry> path;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F(cx * scale, cy * scale), radius * scale, radius * scale), disc.put())) ||
        FAILED(f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F((cx + dx) * scale, (cy + dy) * scale), cut * scale, cut * scale), bite.put())) ||
        FAILED(f->CreatePathGeometry(path.put())) || FAILED(path->Open(sink.put())))
        return;
    disc->CombineWithGeometry(bite.get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.get());
    sink->Close();
    d2d_->FillGeometry(path.get(), Brush(c));
}

void Canvas::Arc(float cx, float cy, float radius, float a0, float a1, D2D1_COLOR_F c, float width)
{
    if (a1 - a0 < 0.5f)
        return;
    D2D1_COLOR_F unused;
    c = InkAt(D2D1::RectF(cx - radius, cy - radius, cx + radius, cy + radius), c, unused);
    winrt::com_ptr<ID2D1Factory> f;
    d2d_->GetFactory(f.put());
    winrt::com_ptr<ID2D1PathGeometry> g;
    winrt::com_ptr<ID2D1GeometrySink> sink;
    if (FAILED(f->CreatePathGeometry(g.put())) || FAILED(g->Open(sink.put())))
        return;
    const float k = 3.14159265f / 180.f, r = radius * scale;
    a1 = std::min(a1, a0 + 359.9f);
    sink->BeginFigure(D2D1::Point2F(cx * scale + r * std::cos(a0 * k), cy * scale + r * std::sin(a0 * k)), D2D1_FIGURE_BEGIN_HOLLOW);
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(cx * scale + r * std::cos(a1 * k), cy * scale + r * std::sin(a1 * k)), D2D1::SizeF(r, r), 0,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, a1 - a0 > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
    sink->EndFigure(D2D1_FIGURE_END_OPEN);
    sink->Close();
    d2d_->DrawGeometry(g.get(), Brush(c), width * scale, RoundStroke(d2d_));
}

// Small text over glass is a step heavier: below 12.5 DIP a regular reads as a medium (D40).
static DWRITE_FONT_WEIGHT GlassWeight(float sizeDip, DWRITE_FONT_WEIGHT w)
{
    return sizeDip <= 12.5f && w <= DWRITE_FONT_WEIGHT_NORMAL ? DWRITE_FONT_WEIGHT_MEDIUM : w;
}

IDWriteTextFormat* Canvas::Format(float px, DWRITE_FONT_WEIGHT w, bool glyph, int align, bool wrap)
{
    auto key = std::make_tuple(int(std::lround(px * 8)), int(w), glyph, align, wrap);
    auto& f = s_.formats_[key];
    if (!f) {
        if (glyph)
            s_.dwrite_->CreateTextFormat(L"Segoe Fluent Icons", nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"", f.put());
        else
            UiTextFormat(s_.dwrite_.get(), px, w, f.put());
        if (!f)
            return nullptr;
        f->SetTextAlignment(align == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER : align == 2 ? DWRITE_TEXT_ALIGNMENT_TRAILING : DWRITE_TEXT_ALIGNMENT_LEADING);
        f->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (!glyph) {
            DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            winrt::com_ptr<IDWriteInlineObject> ellipsis;
            s_.dwrite_->CreateEllipsisTrimmingSign(f.get(), ellipsis.put());
            f->SetTrimming(&trim, ellipsis.get());
        }
    }
    return f.get();
}

float Canvas::DarkAt(D2D1_RECT_F r) const
{
    if (ink.dark.empty())
        return inkDark;
    D2D1_RECT_F p = Px(r);
    D2D1_MATRIX_3X2_F m;
    d2d_->GetTransform(&m);
    return ink.DarkAt(p.left * m._11 + m._31, p.top * m._22 + m._32, p.right * m._11 + m._31, p.bottom * m._22 + m._32, inkDark);
}

D2D1_COLOR_F Canvas::InkAt(D2D1_RECT_F r, D2D1_COLOR_F c, D2D1_COLOR_F& haloInk) const
{
    haloInk = theme.halo;
    if (ink.dark.empty())
        return c;
    // Only the theme's own ink and washes switch (accents and colours stay).
    auto same = [](const D2D1_COLOR_F& a, const D2D1_COLOR_F& b) {
        return std::fabs(a.r - b.r) < 0.01f && std::fabs(a.g - b.g) < 0.01f && std::fabs(a.b - b.b) < 0.01f;
    };
    // Opaque white (a slider's knob, text on an accent) is not ink: the
    // theme's inks are at most 0.96 opaque.
    if (c.a > 0.965f)
        return c;
    bool isText = same(c, theme.text), isWash = !isText && same(c, theme.hover);
    if (!isText && !isWash)
        return c;
    const D2D1_COLOR_F& from = isText ? theme.text : theme.hover;
    D2D1_RECT_F p = Px(r);
    D2D1_MATRIX_3X2_F m;
    d2d_->GetTransform(&m); // groups move and scale what they draw
    float x0 = p.left * m._11 + m._31, y0 = p.top * m._22 + m._32, x1 = p.right * m._11 + m._31, y1 = p.bottom * m._22 + m._32;
    float dark = ink.DarkAt(x0, y0, x1, y1, inkDark);
    if (std::fabs(dark - inkDark) < 0.002f)
        return c;
    Theme t = Theme::Mix(dark);
    haloInk = t.halo;
    const D2D1_COLOR_F& to = isText ? t.text : t.hover;
    return D2D1::ColorF(to.r, to.g, to.b, std::min(1.f, c.a * to.a / std::max(0.01f, from.a)));
}

void Canvas::Text(const std::wstring& s, D2D1_RECT_F r, float size, D2D1_COLOR_F c, DWRITE_FONT_WEIGHT w, int align, bool wrap)
{
    if (s.empty())
        return;
    D2D1_COLOR_F haloInk;
    c = InkAt(r, c, haloInk);
    if (IDWriteTextFormat* f = Format(size * scale, GlassWeight(size, w), false, align, wrap)) {
        D2D1_RECT_F p = Px(r);
        if (halo && haloInk.a > 0) { // the bar's halo: a soft copy just below the ink
            float o = std::max(1.0f, scale * 0.6f);
            D2D1_COLOR_F hc = haloInk;
            hc.a *= c.a;
            DrawUiText(d2d_, s_.dwrite_.get(), s, f, D2D1::RectF(p.left, p.top + o, p.right, p.bottom + o), Brush(hc), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        DrawUiText(d2d_, s_.dwrite_.get(), s, f, p, Brush(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
}

void Canvas::WifiFan(float cx, float cy, float radius, int lit, D2D1_COLOR_F c)
{
    D2D1_COLOR_F unused;
    c = InkAt(D2D1::RectF(cx - radius, cy - radius / 2, cx + radius, cy + radius / 2), c, unused);
    D2D1_COLOR_F off = c;
    off.a *= 0.28f;
    c.a *= alpha_;
    off.a *= alpha_;
    DrawWifiFan(d2d_, Brush(c), cx * scale, cy * scale, radius * scale, lit, c, off);
}

void Canvas::Glyph(const std::wstring& g, D2D1_RECT_F r, float size, D2D1_COLOR_F c)
{
    D2D1_COLOR_F haloInk;
    c = InkAt(r, c, haloInk);
    if (IDWriteTextFormat* f = Format(size * scale, DWRITE_FONT_WEIGHT_NORMAL, true, 1, false)) {
        D2D1_RECT_F p = Px(r);
        if (halo && haloInk.a > 0) {
            float o = std::max(1.0f, scale * 0.6f);
            D2D1_COLOR_F hc = haloInk;
            hc.a *= c.a;
            d2d_->DrawText(g.c_str(), UINT32(g.size()), f, D2D1::RectF(p.left, p.top + o, p.right, p.bottom + o), Brush(hc));
        }
        d2d_->DrawText(g.c_str(), UINT32(g.size()), f, p, Brush(c));
    }
}

float Canvas::TextWidth(const std::wstring& s, float size, DWRITE_FONT_WEIGHT w)
{
    IDWriteTextFormat* f = Format(size * scale, GlassWeight(size, w), false, 0, false);
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (!f || FAILED(UiTextLayout(s_.dwrite_.get(), s, f, 100000, 1000, layout.put())))
        return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.widthIncludingTrailingWhitespace / scale;
}

ID2D1Bitmap* Canvas::Bitmap(const std::wstring& key, const IconImage& image)
{
    auto& b = s_.bitmaps_[key];
    if (!b) {
        if (image.empty())
            return nullptr;
        D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2d_->CreateBitmap(D2D1::SizeU(UINT32(image.width), UINT32(image.height)), image.bgra.data(), UINT32(image.width * 4), &bp,
                                      b.put())))
            return nullptr;
    }
    return b.get();
}

void Canvas::Image(const std::wstring& key, const IconImage& image, D2D1_RECT_F r, float opacity)
{
    if (ID2D1Bitmap* b = Bitmap(key, image))
        d2d_->DrawBitmap(b, Px(r), opacity * alpha_, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
}

void Canvas::ImagePerspective(const std::wstring& key, const IconImage& image, D2D1_RECT_F r, const D2D1_MATRIX_4X4_F& perspective, float opacity)
{
    if (ID2D1Bitmap* b = Bitmap(key, image)) {
        const D2D1_RECT_F px = Px(r);
        d2d_->DrawBitmap(b, &px, opacity * alpha_, D2D1_INTERPOLATION_MODE_LINEAR, nullptr, &perspective);
    }
}

bool Canvas::Cached(const std::wstring& key) const
{
    auto it = s_.bitmaps_.find(key);
    return it != s_.bitmaps_.end() && it->second;
}

D2D1_RECT_F Canvas::Caret(const std::wstring& s, D2D1_RECT_F r, float size, DWRITE_FONT_WEIGHT w, size_t pos)
{
    D2D1_RECT_F fallback = D2D1::RectF(r.left, r.top, r.left + 1.5f, r.top + size * 1.33f);
    IDWriteTextFormat* f = Format(size * scale, GlassWeight(size, w), false, 0, true);
    // A zero-width space after the text: a caret after a final line break sits on the new line.
    std::wstring text = s + L"\u200B";
    winrt::com_ptr<IDWriteTextLayout> layout;
    if (!f || FAILED(UiTextLayout(s_.dwrite_.get(), text, f, (r.right - r.left) * scale, 1e6f, layout.put())))
        return fallback;
    float x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS m{};
    if (FAILED(layout->HitTestTextPosition(UINT32(std::min(pos, s.size())), FALSE, &x, &y, &m)))
        return fallback;
    return D2D1::RectF(r.left + x / scale, r.top + y / scale, r.left + x / scale + 1.5f, r.top + (y + m.height) / scale);
}

void Canvas::ImageRounded(const std::wstring& key, const IconImage& image, D2D1_RECT_F r, float radius)
{
    if (!Cached(key)) Image(key, image, D2D1::RectF(), 0);
    auto found = s_.bitmaps_.find(key);
    if (found == s_.bitmaps_.end() || !found->second) return;
    const auto p = Px(r);
    if (!s_.roundedImageBrush_ && FAILED(d2d_->CreateBitmapBrush(found->second.get(), s_.roundedImageBrush_.put()))) return;
    auto* brush = s_.roundedImageBrush_.get();
    brush->SetBitmap(found->second.get());
    const auto size = found->second->GetSize();
    brush->SetTransform(D2D1::Matrix3x2F::Scale((p.right - p.left) / size.width, (p.bottom - p.top) / size.height) *
                        D2D1::Matrix3x2F::Translation(p.left, p.top));
    brush->SetOpacity(alpha_);
    FillContinuousRect(d2d_, p, radius * scale, brush);
}

void Canvas::ImageCircle(const std::wstring& key, const IconImage& image, D2D1_RECT_F r)
{
    winrt::com_ptr<ID2D1Factory> f;
    d2d_->GetFactory(f.put());
    winrt::com_ptr<ID2D1EllipseGeometry> g;
    D2D1_RECT_F p = Px(r);
    if (FAILED(f->CreateEllipseGeometry(D2D1::Ellipse(D2D1::Point2F((p.left + p.right) / 2, (p.top + p.bottom) / 2), (p.right - p.left) / 2,
                                                      (p.bottom - p.top) / 2),
                                        g.put())))
        return;
    d2d_->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), g.get(), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE), nullptr);
    Image(key, image, r);
    d2d_->PopLayer();
}

void Canvas::Ripple(D2D1_RECT_F r, float radius, float x, float y, float t, D2D1_COLOR_F c)
{
    t = std::clamp(t, 0.f, 1.f);
    const D2D1_RECT_F p = Px(r);
    float reach = std::hypot(r.right - r.left, r.bottom - r.top) * scale;
    float rr = std::max(1.f, reach * (1 - (1 - t) * (1 - t) * (1 - t)));
    float a = c.a * alpha_ * (1 - t) * (1 - t);
    D2D1_GRADIENT_STOP stops[] = {{0.f, D2D1::ColorF(c.r, c.g, c.b, a * 0.55f)}, {0.75f, D2D1::ColorF(c.r, c.g, c.b, a)}, {1.f, D2D1::ColorF(c.r, c.g, c.b, 0)}};
    winrt::com_ptr<ID2D1GradientStopCollection> coll;
    winrt::com_ptr<ID2D1RadialGradientBrush> brush;
    D2D1_POINT_2F centre = D2D1::Point2F(x * scale, y * scale);
    if (FAILED(d2d_->CreateGradientStopCollection(stops, 3, coll.put())) ||
        FAILED(d2d_->CreateRadialGradientBrush(D2D1::RadialGradientBrushProperties(centre, D2D1::Point2F(), rr, rr), coll.get(), brush.put())))
        return;
    FillContinuousRect(d2d_, p, radius * scale, brush.get());
}

void Canvas::BeginBlur()
{
    // Recorded into a command list, drawn through a Gaussian blur at EndBlur.
    blurList_ = nullptr;
    blurTarget_ = nullptr;
    if (FAILED(d2d_->CreateCommandList(blurList_.put())))
        return;
    d2d_->GetTarget(blurTarget_.put());
    d2d_->SetTarget(blurList_.get());
}

void Canvas::EndBlur(float radius)
{
    if (!blurList_ || !blurTarget_)
        return;
    d2d_->SetTarget(blurTarget_.get());
    winrt::com_ptr<ID2D1CommandList> list = std::move(blurList_);
    blurTarget_ = nullptr;
    if (FAILED(list->Close()))
        return;
    D2D1_MATRIX_3X2_F transform;
    d2d_->GetTransform(&transform);
    d2d_->SetTransform(D2D1::Matrix3x2F::Identity()); // the commands carry their own transforms
    if (radius > 0.05f && (s_.blur_ || SUCCEEDED(d2d_->CreateEffect(CLSID_D2D1GaussianBlur, s_.blur_.put())))) {
        s_.blur_->SetInput(0, list.get());
        s_.blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_STANDARD_DEVIATION, radius * scale);
        s_.blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_BORDER_MODE, D2D1_BORDER_MODE_SOFT);
        s_.blur_->SetValue(D2D1_GAUSSIANBLUR_PROP_OPTIMIZATION, D2D1_GAUSSIANBLUR_OPTIMIZATION_SPEED); // downsampled: a frame's budget
        d2d_->DrawImage(s_.blur_.get());
        s_.blur_->SetInput(0, nullptr);
    } else {
        d2d_->DrawImage(list.get());
    }
    d2d_->SetTransform(transform);
}

void Canvas::Clip(D2D1_RECT_F r) { d2d_->PushAxisAlignedClip(Px(r), D2D1_ANTIALIAS_MODE_ALIASED); }
void Canvas::Unclip() { d2d_->PopAxisAlignedClip(); }

void Canvas::MaskRounded(D2D1_RECT_F r, float radius)
{
    const auto p = Px(r);
    const float rp = radius * scale;
    const auto& old = s_.roundedMaskRect_;
    if (!s_.roundedMask_ || old.left != p.left || old.top != p.top || old.right != p.right || old.bottom != p.bottom ||
        s_.roundedMaskRadius_ != rp) {
        winrt::com_ptr<ID2D1CommandList> mask;
        if (FAILED(d2d_->CreateCommandList(mask.put())))
            return;
        winrt::com_ptr<ID2D1Image> target;
        D2D1_MATRIX_3X2_F transform;
        d2d_->GetTarget(target.put());
        d2d_->GetTransform(&transform);
        d2d_->SetTarget(mask.get());
        d2d_->SetTransform(D2D1::Matrix3x2F::Identity());
        FillContinuousRect(d2d_, p, rp, Brush(D2D1::ColorF(1, 1, 1, 1)));
        d2d_->SetTarget(target.get());
        d2d_->SetTransform(transform);
        if (FAILED(mask->Close()))
            return;
        s_.roundedMask_ = std::move(mask);
        s_.roundedMaskRect_ = p;
        s_.roundedMaskRadius_ = rp;
    }
    d2d_->DrawImage(s_.roundedMask_.get(), nullptr, nullptr, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR,
                   D2D1_COMPOSITE_MODE_DESTINATION_IN);
}

void Canvas::Continue() { s_.frameWanted_ = true; }

// Opacity multiplies into every colour drawn inside the group instead of a
// D2D layer: layers allocate intermediate surfaces, which WARP keeps cached
// (tens of MB with a fading tile per row). Overlapping shapes in a group fade
// individually, which these groups never notice.
void Canvas::BeginGroup(float opacity, float k, float cx, float cy, float dx, float dy)
{
    D2D1_MATRIX_3X2_F current;
    d2d_->GetTransform(&current);
    groups_.push_back({current, alpha_});
    D2D1_MATRIX_3X2_F local = D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(cx * scale, cy * scale)) * D2D1::Matrix3x2F::Translation(dx * scale, dy * scale);
    d2d_->SetTransform(local * current);
    alpha_ *= std::clamp(opacity, 0.f, 1.f);
}

void Canvas::EndGroup()
{
    if (groups_.empty())
        return;
    d2d_->SetTransform(groups_.back().first);
    alpha_ = groups_.back().second;
    groups_.pop_back();
}

// ---- Surface -----------------------------------------------------------------

struct Surface::Shared {
    winrt::com_ptr<ID3D11Device> d3d;
    winrt::com_ptr<IDXGIFactory2> dxgi;
    winrt::com_ptr<IDCompositionDevice> dcomp;
    winrt::com_ptr<ID2D1Factory1> d2dFactory;
    winrt::com_ptr<ID2D1Device> d2d;
    bool lost = false;
    ~Shared() { if (d2d) ForgetContinuousMaskDomain(d2d.as<IUnknown>().get()); }
};

std::shared_ptr<Surface::Shared> Surface::SharedDevice()
{
    static std::weak_ptr<Shared> cache;
    if (auto s = cache.lock(); s && !s->lost)
        return s;
    auto s = std::make_shared<Shared>();
    // WARP (software): popups are small 2D content, and a second hardware
    // device costs 70-100 MB of driver memory and wakes the GPU (V16).
    D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2,
                                            D3D11_SDK_VERSION, s->d3d.put(), nullptr, nullptr))) {
        Log(L"surface: no D3D11 device");
        return nullptr;
    }
    auto dxgiDevice = s->d3d.as<IDXGIDevice>();
    winrt::com_ptr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(adapter.put());
    adapter->GetParent(IID_PPV_ARGS(s->dxgi.put()));
    D2D1_FACTORY_OPTIONS fo{};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo, s->d2dFactory.put_void())) ||
        FAILED(s->d2dFactory->CreateDevice(dxgiDevice.get(), s->d2d.put())) ||
        FAILED(DCompositionCreateDevice(dxgiDevice.get(), IID_PPV_ARGS(s->dcomp.put())))) {
        Log(L"surface: device setup failed");
        return nullptr;
    }
    cache = s;
    return s;
}

Surface::Surface(HINSTANCE instance, const wchar_t* title, bool activatable, float insetDip, bool desktopLayer)
    : activatable_(activatable), desktopLayer_(desktopLayer), insetDip_(insetDip)
{
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.hInstance = instance;
        wc.lpfnWndProc = Proc;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        RegisterClassExW(&wc);
        registered = true;
    }
    DWORD ex = WS_EX_TOOLWINDOW | (desktopLayer ? 0 : WS_EX_TOPMOST) | WS_EX_NOREDIRECTIONBITMAP | (activatable ? 0 : WS_EX_NOACTIVATE);
    hwnd_ = CreateWindowExW(ex, kClass, title, WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    // The glass below the content samples the screen: the window must not be
    // in what it samples (Desktop Duplication always leaves it out; other
    // captures only while the bar uses Windows.Graphics.Capture).
    all_.push_back(this);
    ApplyCaptureExclusion();
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), reinterpret_cast<IUnknown**>(dwrite_.put()));
}

Surface::~Surface()
{
    all_.erase(std::remove(all_.begin(), all_.end(), this), all_.end());
    if (hwnd_) {
        if (host_) {
            GlassPanel p;
            p.hwnd = hwnd_;
            host_->SetPanel(p); // not visible: removes the layer
        }
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
    }
}

void Surface::SetGlassHost(GlassHost* host) { host_ = host; }
void Surface::SetCapturable(bool capturable) { capturable_ = capturable; }

void Surface::ApplyCaptureExclusion() { SetCaptureExclusion(hwnd_, (capturable_ || captureVisible_) && !private_); }

void Surface::SetCaptureVisible(bool visible)
{
    captureVisible_ = visible;
    for (Surface* s : all_)
        s->ApplyCaptureExclusion();
}

void Surface::SetPrivate(bool on)
{
    if (on == private_)
        return;
    private_ = on;
    ApplyCaptureExclusion();
}

int Surface::InsetPx() const { return int(std::lround(insetDip_ * scale_)); }

D2D1_SIZE_U Surface::BufferSize() const
{
    const int inset = 2 * InsetPx();
    return D2D1::SizeU(UINT32(std::max({1, int(window_.right - window_.left), int(std::lround(resizeReserve_.width * scale_)) + inset})),
                      UINT32(std::max({1, int(window_.bottom - window_.top), int(std::lround(resizeReserve_.height * scale_)) + inset})));
}

POINT Surface::ScreenToContent(POINT p) const { return POINT{p.x - rect_.left, p.y - rect_.top}; }
POINT Surface::ContentToScreen(POINT p) const { return POINT{p.x + rect_.left, p.y + rect_.top}; }

void Surface::SetRadius(float dip)
{
    if (radius_ == dip)
        return;
    radius_ = dip;
    if (visible_)
        PublishPanel();
}

void Surface::SetShapes(const std::vector<ShapeDip>& shapes, bool animating)
{
    auto same = [](const ShapeDip& a, const ShapeDip& b) {
        auto sameRect = [](const D2D1_RECT_F& x, const D2D1_RECT_F& y) { return x.left == y.left && x.top == y.top && x.right == y.right && x.bottom == y.bottom; };
        auto samePart = [&](const ShapeDip::Part& x, const ShapeDip::Part& y) { return sameRect(x.rect, y.rect) && std::equal(x.radii, x.radii + 4, y.radii); };
        return a.radius == b.radius && a.fade == b.fade && sameRect(a.rect, b.rect) && a.melt == b.melt && a.partMix == b.partMix &&
               a.tailX == b.tailX && a.tailHalfWidth == b.tailHalfWidth && a.tailHeight == b.tailHeight && a.tailInset == b.tailInset &&
               a.parts.size() == b.parts.size() && std::equal(a.parts.begin(), a.parts.end(), b.parts.begin(), samePart);
    };
    if (!animating && !shapesAnimating_ && shapes.size() == shapes_.size() && std::equal(shapes.begin(), shapes.end(), shapes_.begin(), same))
        return;
    shapes_ = shapes;
    shapesAnimating_ = animating;
    if (visible_ || hiding_)
        PublishPanel();
}

void Surface::PublishPanel()
{
    if (!host_ || glassless_)
        return;
    GlassPanel p;
    p.hwnd = hwnd_;
    p.gen = ++glassGen_;
    glassPending_ = true;
    p.window = window_;
    const auto buffer = BufferSize();
    p.bufferW = int(buffer.width);
    p.bufferH = int(buffer.height);
    p.inset = float(InsetPx());
    p.radius = radius_ * scale_;
    for (const ShapeDip& s : shapes_) {
        // Whole px, so the glass and the content's washes line up.
        auto px = [&](float v) { return std::round(v * scale_) + p.inset; };
        GlassShape g{px(s.rect.left), px(s.rect.top), px(s.rect.right), px(s.rect.bottom), s.radius * scale_, s.fade};
        for (const ShapeDip::Part& part : s.parts)
            g.parts.push_back({px(part.rect.left), px(part.rect.top), px(part.rect.right), px(part.rect.bottom),
                               {part.radii[0] * scale_, part.radii[1] * scale_, part.radii[2] * scale_, part.radii[3] * scale_}});
        g.melt = s.melt * scale_;
        g.partMix = s.partMix;
        g.tailX = px(s.tailX);
        g.tailHalfWidth = s.tailHalfWidth * scale_;
        g.tailHeight = s.tailHeight * scale_;
        g.tailInset = s.tailInset * scale_;
        p.shapes.push_back(std::move(g));
    }
    p.visible = visible_ || hiding_;
    p.desktop = desktopLayer_;
    if (desktopLayer_)
        p.offset = POINT{window_.left - nativeWindow_.left, window_.top - nativeWindow_.top};
    p.animating = shapesAnimating_;
    p.pop = pop_;
    p.popOpen = popOpen_;
    p.popX = float(anchor_.x - (desktopLayer_ ? nativeWindow_.left : window_.left));
    p.popY = float(anchor_.y - (desktopLayer_ ? nativeWindow_.top : window_.top));
    p.rippleX = rippleX_;
    p.rippleY = rippleY_;
    p.rippleMs = rippleMs_;
    host_->SetPanel(p);
}

void Surface::SetGlassless(bool on)
{
    if (on == glassless_)
        return;
    if (on) {
        // Withdraw the glass layer while it is still published, then paint without waiting for it.
        const bool visible = visible_, hiding = hiding_;
        visible_ = hiding_ = false;
        PublishPanel();
        visible_ = visible;
        hiding_ = hiding;
        glassless_ = true;
        glassPending_ = false;
        glassReady_ = true;
        KillTimer(hwnd_, kGlassWaitTimer);
        Invalidate();
        return;
    }
    glassless_ = false;
    if (!visible_)
        return;
    // As on Show: the content waits a little for its glass, then a plain panel stands in.
    glassReady_ = false;
    shownMs_ = ClockMs();
    SetTimer(hwnd_, kGlassWaitTimer, UINT(kGlassWaitMs) + 20, nullptr);
    PublishPanel();
    Invalidate();
}

void Surface::Ripple(float x, float y)
{
    int inset = InsetPx();
    rippleX_ = x * scale_ + float(inset);
    rippleY_ = y * scale_ + float(inset);
    rippleMs_ = ClockMs();
    PublishPanel();
}

LRESULT CALLBACK Surface::Proc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    auto* self = reinterpret_cast<Surface*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (!self)
        return DefWindowProcW(h, m, w, l);
    switch (m) {
    case WM_TIMER:
        if (w == kHideTimer) {
            self->FinishHide();
            return 0;
        }
        if (w == kGlassWaitTimer) {
            KillTimer(h, kGlassWaitTimer);
            self->Invalidate(); // a plain panel if the glass still has not come
            if (self->onReady && !self->glassReady_)
                self->onReady();
            return 0;
        }
        break;
    case WM_SURFACE_FRAME:
        if (self->visible_)
            self->Paint();
        return 0;
    case WM_GLASS_PRESENTED:
        if (self->presentPending_ && unsigned(w) >= self->presentGen_)
            self->Present();
        return 0;
    case WM_GLASS_READY:
        if (!self->glassReady_ && self->visible_) {
            self->glassReady_ = true;
            KillTimer(h, kGlassWaitTimer);
            self->Invalidate();
            if (self->onReady)
                self->onReady();
        }
        return 0;
    case WM_GLASS_INK:
        self->SetInk(w != 0, InkFor(h));
        return 0;
    }
    // Mouse positions in content coordinates; presses on the margin are the caller's to decide.
    const bool mouse = (m >= WM_MOUSEFIRST && m <= WM_MBUTTONDBLCLK && m != WM_MOUSEWHEEL) || m == WM_MOUSEHWHEEL;
    if (mouse && m != WM_MOUSEHWHEEL) {
        int inset = self->InsetPx();
        int x = GET_X_LPARAM(l) - inset, y = GET_Y_LPARAM(l) - inset;
        if (self->desktopLayer_) {
            x -= self->window_.left - self->nativeWindow_.left;
            y -= self->window_.top - self->nativeWindow_.top;
        }
        bool outside = x < 0 || y < 0 || x >= self->rect_.right - self->rect_.left || y >= self->rect_.bottom - self->rect_.top;
        if (outside && (m == WM_LBUTTONDOWN || m == WM_RBUTTONDOWN || m == WM_MBUTTONDOWN)) {
            if (self->onDismiss)
                self->onDismiss();
            return 0;
        }
        if (self->hiding_)
            return 0;
        l = MAKELPARAM(x, y);
    }
    if (self->message) {
        LRESULT result = 0;
        if (self->message(m, w, l, result))
            return result;
    }
    switch (m) {
    case WM_MOUSEACTIVATE:
        return self->activatable_ ? MA_ACTIVATE : MA_NOACTIVATE;
    case WM_PAINT:
        ValidateRect(h, nullptr);
        self->Paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DPICHANGED:
        self->Invalidate();
        return 0;
    case WM_CLOSE:
        if (self->onDismiss)
            self->onDismiss();
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

void Surface::Place(RECT rect)
{
    rect_ = rect;
    scale_ = PopupScale(rect);
    int inset = InsetPx();
    window_ = RECT{rect.left - inset, rect.top - inset, rect.right + inset, rect.bottom + inset};
    ApplyPlacement();
}

void Surface::ApplyPlacement()
{
    if (desktopLayer_) {
        MONITORINFO monitor{sizeof(monitor)};
        GetMonitorInfoW(MonitorFromRect(&rect_, MONITOR_DEFAULTTONEAREST), &monitor);
        if (!EqualRect(&nativeWindow_, &monitor.rcMonitor)) {
            nativeWindow_ = monitor.rcMonitor;
            SetWindowPos(hwnd_, nullptr, nativeWindow_.left, nativeWindow_.top,
                         nativeWindow_.right - nativeWindow_.left, nativeWindow_.bottom - nativeWindow_.top, SWP_NOACTIVATE | SWP_NOZORDER);
            inputRegion_ = {};
        }
        // The host has no full-screen bitmap. Its region admits only the
        // widget and shadow; include the previous frame while it is in flight.
        RECT region = window_;
        if (!IsRectEmpty(&presentedWindow_))
            UnionRect(&region, &region, &presentedWindow_);
        OffsetRect(&region, -nativeWindow_.left, -nativeWindow_.top);
        if (!EqualRect(&region, &inputRegion_)) {
            HRGN shape = CreateRectRgn(region.left, region.top, region.right, region.bottom);
            if (SetWindowRgn(hwnd_, shape, FALSE))
                inputRegion_ = region; // Windows owns the region after success
            else
                DeleteObject(shape);
        }
        return;
    }
    SetWindowPos(hwnd_, desktopLayer_ ? nullptr : HWND_TOPMOST, window_.left, window_.top, window_.right - window_.left, window_.bottom - window_.top,
                 SWP_NOACTIVATE | (desktopLayer_ ? SWP_NOZORDER : 0));
    ApplyRegion();
}

void Surface::SetRegion(D2D1_RECT_F dip)
{
    region_ = dip;
    ApplyRegion();
}

void Surface::ApplyRegion()
{
    if (desktopLayer_)
        return; // its own region (ApplyPlacement)
    RECT r{};
    if (region_.right > region_.left && region_.bottom > region_.top) {
        const int inset = InsetPx();
        r = RECT{LONG(std::floor(region_.left * scale_)), LONG(std::floor(region_.top * scale_)), LONG(std::ceil(region_.right * scale_)) + 2 * inset,
                 LONG(std::ceil(region_.bottom * scale_)) + 2 * inset};
    }
    if (EqualRect(&r, &inputRegion_))
        return;
    HRGN shape = IsRectEmpty(&r) ? nullptr : CreateRectRgn(r.left, r.top, r.right, r.bottom);
    if (SetWindowRgn(hwnd_, shape, FALSE))
        inputRegion_ = r; // Windows owns the region after success
    else if (shape)
        DeleteObject(shape);
}

void Surface::Show(RECT rect, bool activate, POINT anchor, bool pop)
{
    KillTimer(hwnd_, kHideTimer);
    Place(rect);
    anchor_ = anchor;
    bool wasHiding = hiding_;
    hiding_ = false;
    if (!visible_) {
        visible_ = true;
        firstPaint_ = !wasHiding;
        if (!wasHiding) {
            glassReady_ = glassless_; // nothing to wait for
            shownMs_ = ClockMs();
            SetTimer(hwnd_, kGlassWaitTimer, UINT(kGlassWaitMs) + 20, nullptr);
        }
        ShowWindow(hwnd_, activate && activatable_ ? SW_SHOW : SW_SHOWNOACTIVATE);
    }
    if (!desktopLayer_)
        SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | (activate && activatable_ ? 0 : SWP_NOACTIVATE));
    if (activate && activatable_ && !SetForegroundWindow(hwnd_))
        SwitchToThisWindow(hwnd_, TRUE);
    // Content and glass pop out of the anchor together (DWM runs both
    // animations). The animation is in place before the first frame shows.
    if (pop) {
        ++pop_;
        popOpen_ = true;
        if (EnsureDevice())
            PopVisual(shared_->dcomp.get(), visual_.get(), true,
                      float(anchor.x - (desktopLayer_ ? nativeWindow_.left : window_.left)),
                      float(anchor.y - (desktopLayer_ ? nativeWindow_.top : window_.top)));
    } else {
        popOpen_ = true;
        EnsureDevice();
    }
    Paint();
    PublishPanel();
}

void Surface::Move(RECT rect)
{
    // Keep each desktop content frame paired with its glass geometry.
    // Input can replace the next request
    // while the current one is in flight, without waiting on the UI thread.
    if (desktopLayer_ && visible_ && glassReady_ && presentPending_) {
        nextResize_ = rect;
        frameWanted_ = true;
        return;
    }
    if (EqualRect(&rect, &rect_))
        return;
    bool resized = rect.right - rect.left != rect_.right - rect_.left || rect.bottom - rect.top != rect_.bottom - rect_.top;
    Place(rect);
    if (visible_) {
        PublishPanel();
        if (resized || desktopLayer_) // desktop translation travels with the pixel buffer
            Invalidate();
    }
}

void Surface::ReserveResize(D2D1_SIZE_F sizeDip)
{
    const bool release = sizeDip.width == 0 && sizeDip.height == 0;
    releaseResize_ = release && (presentPending_ || nextResize_.has_value());
    if (releaseResize_)
        return; // first finish the final geometry frame, then return its spare buffers
    if (resizeReserve_.width == sizeDip.width && resizeReserve_.height == sizeDip.height)
        return;
    resizeReserve_ = sizeDip;
    presentPending_ = false;
    if (visible_) {
        PublishPanel();
        Invalidate();
    }
}

void Surface::Hide(bool animate)
{
    if (!visible_)
        return;
    visible_ = false;
    presentPending_ = false;
    presentedWindow_ = {};
    nextResize_.reset();
    releaseResize_ = false;
    shapesAnimating_ = false;
    burst_ = Burst{}; // time spent hidden is not an animation gap
    if (!animate || !visual_ || !shared_) {
        FinishHide();
        return;
    }
    hiding_ = true;
    ++pop_;
    popOpen_ = false;
    PopVisual(shared_->dcomp.get(), visual_.get(), false,
              float(anchor_.x - (desktopLayer_ ? nativeWindow_.left : window_.left)),
              float(anchor_.y - (desktopLayer_ ? nativeWindow_.top : window_.top)));
    PublishPanel();
    SetTimer(hwnd_, kHideTimer, kHideMs, nullptr);
}

void Surface::FinishHide()
{
    KillTimer(hwnd_, kHideTimer);
    KillTimer(hwnd_, kGlassWaitTimer);
    hiding_ = false;
    visible_ = false;
    glassReady_ = false;
    ShowWindow(hwnd_, SW_HIDE);
    PublishPanel(); // not visible: the glass layer goes too
    ResetDevice();  // the swap chain is recreated on the next Show
}

void Surface::SetInk(bool dark, const InkGrid& grid)
{
    double now = ClockMs();
    // Not on screen yet (or a new grid): the ink is just taken.
    bool snap = !visible_ || !glassReady_;
    darkInk_ = dark;
    if (snap)
        inkAll_ = InkFade{};
    inkAll_.Target(dark ? 1.f : 0.f, now);
    if (snap || grid.w != inkGrid_.w || grid.h != inkGrid_.h || grid.dark.size() != inkCells_.size())
        inkCells_.assign(grid.dark.size(), InkFade{});
    inkGrid_ = grid;
    for (size_t i = 0; i < inkCells_.size(); ++i)
        inkCells_[i].Target(grid.dark[i] ? 1.f : 0.f, now);
    Invalidate();
}

void Surface::Invalidate()
{
    if (visible_)
        Paint();
}

void Surface::ResetDevice()
{
    presentPending_ = false;
    presentedWindow_ = {};
    nextResize_.reset();
    releaseResize_ = false;
    d2d_ = nullptr;
    if (target_)
        target_->SetRoot(nullptr);
    visual_ = nullptr;
    target_ = nullptr;
    backTarget_ = nullptr;
    swap_ = nullptr;
    brush_ = nullptr;
    calloutMask_ = nullptr;
    roundedImageBrush_ = nullptr;
    roundedMask_ = nullptr;
    bitmaps_.clear();
    bufW_ = bufH_ = 0;
    if (shared_)
        shared_->dcomp->Commit();
    shared_ = nullptr;
}

bool Surface::EnsureDevice()
{
    if (d2d_)
        return true;
    shared_ = SharedDevice();
    if (!shared_)
        return false;
    const auto size = BufferSize();
    int w = int(size.width), h = int(size.height);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = UINT(w);
    desc.Height = UINT(h);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
    auto& s = *shared_;
    if (FAILED(s.dxgi->CreateSwapChainForComposition(s.d3d.get(), &desc, nullptr, swap_.put())) ||
        FAILED(s.dcomp->CreateTargetForHwnd(hwnd_, TRUE, target_.put())) || FAILED(s.dcomp->CreateVisual(visual_.put())) ||
        FAILED(visual_->SetContent(swap_.get())) || FAILED(target_->SetRoot(visual_.get())) || FAILED(s.dcomp->Commit()) ||
        FAILED(s.d2d->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, d2d_.put()))) {
        Log(L"surface: swap chain setup failed");
        s.lost = true; // a fresh shared device next time
        ResetDevice();
        return false;
    }
    bufW_ = w;
    bufH_ = h;
    return true;
}

void Surface::Paint()
{
    if (presentPending_) {
        // Keep this buffer paired with its glass. Input and time continue to
        // advance; the next frame uses their latest state once it is presented.
        frameWanted_ = true;
        return;
    }
    if (!visible_ || !EnsureDevice())
        return;
    const auto size = BufferSize();
    int w = int(size.width), h = int(size.height);
    if (w != bufW_ || h != bufH_) {
        d2d_->SetTarget(nullptr);
        backTarget_ = nullptr;
        if (FAILED(swap_->ResizeBuffers(2, UINT(w), UINT(h), DXGI_FORMAT_B8G8R8A8_UNORM, 0))) {
            ResetDevice();
            return;
        }
        bufW_ = w;
        bufH_ = h;
    }
    if (!backTarget_) {
        winrt::com_ptr<IDXGISurface> back;
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(swap_->GetBuffer(0, IID_PPV_ARGS(back.put()))) || FAILED(d2d_->CreateBitmapFromDxgiSurface(back.get(), &bp, backTarget_.put()))) {
            ResetDevice();
            return;
        }
    }
    LARGE_INTEGER t0, t1, f;
    QueryPerformanceCounter(&t0);
    d2d_->SetTarget(backTarget_.get());
    d2d_->BeginDraw();
    d2d_->Clear(D2D1::ColorF(0, 0, 0, 0));
    d2d_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    frameWanted_ = false;
    if (paint) {
        float inset = float(InsetPx());
        D2D1_MATRIX_3X2_F base = D2D1::Matrix3x2F::Translation(inset, inset);
        d2d_->SetTransform(base);
        Canvas c(*this, d2d_.get(), scale_, base);
        c.now = ClockMs();
        c.panel = !host_ || (!glassReady_ && c.now - shownMs_ > kGlassWaitMs);
        if (c.panel) {
            c.theme = Theme::Panel();
            c.inkDark = c.theme.light ? 1.f : 0.f;
        } else {
            // The inks fade to what the glass needs now (InkFade).
            bool fading = inkAll_.Moving(c.now);
            c.inkDark = inkAll_.set ? inkAll_.Value(c.now) : (darkInk_ ? 1.f : 0.f);
            c.theme = Theme::Mix(c.inkDark);
            if (!inkCells_.empty()) {
                c.ink.w = inkGrid_.w;
                c.ink.h = inkGrid_.h;
                c.ink.cellX = inkGrid_.cellX;
                c.ink.cellY = inkGrid_.cellY;
                c.ink.dark.resize(inkCells_.size());
                for (size_t i = 0; i < inkCells_.size(); ++i) {
                    c.ink.dark[i] = inkCells_[i].Value(c.now);
                    fading |= inkCells_[i].Moving(c.now);
                }
            }
            if (fading)
                frameWanted_ = true;
        }
        paint(c);
        d2d_->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    HRESULT hr = d2d_->EndDraw();
    d2d_->SetTarget(nullptr);
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    double ms = double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart);
    if (firstPaint_ || ms > 16) {
        wchar_t title[64] = {};
        GetWindowTextW(hwnd_, title, 64);
        Log(L"surface: %s painted %dx%d in %.1f ms%s", title, w, h, ms, firstPaint_ ? L" (first after show)" : L"");
        firstPaint_ = false;
    }
    if (FAILED(hr)) {
        Log(L"surface: paint failed 0x%08X, recreating", unsigned(hr));
        if (shared_)
            shared_->lost = true;
        ResetDevice();
        return;
    }
    paintMs_ = ms;
    presentGen_ = glassGen_;
    presentPending_ = true;
    bool waitForGlass = glassPending_ && glassReady_ && !IsGlassPresented(hwnd_, presentGen_);
    glassPending_ = false;
    if (!waitForGlass)
        Present();
}

void Surface::Present()
{
    if (!visible_ || !presentPending_ || !swap_)
        return;
    if (desktopLayer_) {
        ApplyPlacement();
        const DXGI_MATRIX_3X2_F matrix{1, 0, 0, 1, float(window_.left - nativeWindow_.left), float(window_.top - nativeWindow_.top)};
        if (FAILED(swap_.as<IDXGISwapChain2>()->SetMatrixTransform(&matrix))) {
            ResetDevice();
            return;
        }
        // One composed frame later the region can forget the previous bounds.
        frameWanted_ |= !EqualRect(&presentedWindow_, &window_);
        presentedWindow_ = window_;
    }
    presentPending_ = false;
    HRESULT hr = swap_->Present(0, 0);
    if (FAILED(hr)) {
        Log(L"surface: present failed 0x%08X, recreating", unsigned(hr));
        if (shared_)
            shared_->lost = true;
        ResetDevice();
        return;
    }
    const double ms = paintMs_;
    // Frame statistics of an animation (a run of frames that asked for more).
    double end = ClockMs();
    if (frameWanted_) {
        if (!burst_.frames)
            burst_ = Burst{end, end, 0, 0, 0};
        burst_.maxGap = std::max(burst_.maxGap, end - burst_.last);
        burst_.maxPaint = std::max(burst_.maxPaint, ms);
        burst_.last = end;
        ++burst_.frames;
    } else if (burst_.frames) {
        if (burst_.frames >= 4) {
            wchar_t title[64] = {};
            GetWindowTextW(hwnd_, title, 64);
            double span = end - burst_.start;
            Log(L"surface: %s animated %d frames in %.0f ms (%.0f fps), longest gap %.1f ms, slowest paint %.1f ms", title, burst_.frames + 1, span,
                span > 0 ? 1000.0 * burst_.frames / span : 0.0, std::max(burst_.maxGap, end - burst_.last), std::max(burst_.maxPaint, ms));
        }
        burst_ = Burst{};
    }
    if (nextResize_) {
        RECT next = *nextResize_;
        nextResize_.reset();
        Move(next);
    }
    if (releaseResize_ && !presentPending_ && !nextResize_)
        ReserveResize(D2D1::SizeF());
    if (frameWanted_ && visible_)
        FramePump::Get().Want(hwnd_);
}

} // namespace atrium
