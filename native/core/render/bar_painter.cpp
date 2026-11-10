#include "bar_painter.h"
#include "continuous_corners.h"

#include "round_stroke.h"
#include "ui_font.h"
#include "wifi_glyph.h"
#include "../log.h"
#include "../motion.h"
#include "../settings.h"
#include "glass_renderer.h"

#include <algorithm>
#include <cmath>
#include <set>

#pragma comment(lib, "dxguid.lib") // CLSID_D2D1Shadow

namespace atrium {

namespace {

// Positions (items, the bar's edges) ease towards new places at this rate per second (about 0.3 s).
constexpr float kSlideRate = 14;

D2D1_COLOR_F InkColor(float dark)
{
    return D2D1::ColorF(1.f - 0.93f * dark, 1.f - 0.93f * dark, 1.f - 0.91f * dark, 0.96f - 0.04f * dark);
}

// motion::Approach, landing on `target` once the rest is invisible: 0..1 values.
float Approach(float v, float target, double dtMs, float rate)
{
    v = motion::Approach(v, target, dtMs, rate);
    return std::fabs(target - v) < 0.002f ? target : v;
}

// The same for positions in px.
float ApproachPx(float v, float target, double dtMs, float rate)
{
    v = motion::Approach(v, target, dtMs, rate);
    return std::fabs(target - v) < 0.25f ? target : v;
}


} // namespace

BarPainter::BarPainter(ID2D1DeviceContext2* d2d, IDWriteFactory3* dwrite)
{
    dwrite_.copy_from(dwrite);
    winrt::check_hresult(d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 1), contentBrush_.put()));
    winrt::check_hresult(d2d->CreateEffect(CLSID_D2D1Shadow, shadow_.put()));
}

IDWriteTextFormat* BarPainter::Format(float px, DWRITE_FONT_WEIGHT weight, const wchar_t* family)
{
    auto key = std::make_tuple(int(std::lround(px * 4)), int(weight), std::wstring(family ? family : L""));
    auto& f = formats_[key];
    if (!f) {
        if (family)
            dwrite_->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px, L"", f.put());
        else
            UiTextFormat(dwrite_.get(), px, weight, f.put());
        if (f) {
            f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        }
    }
    return f.get();
}

ID2D1Bitmap1* BarPainter::BitmapFor(ID2D1DeviceContext2* d2d, const SceneItem& item)
{
    if (!item.image || item.image->empty())
        return nullptr;
    auto& b = bitmaps_[item.imageId];
    if (!b) {
        const IconImage& img = *item.image;
        D2D1_BITMAP_PROPERTIES1 bp =
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_NONE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        d2d->CreateBitmap(D2D1::SizeU(UINT32(img.width), UINT32(img.height)), img.bgra.data(), UINT32(img.width * 4), &bp, b.put());
    }
    return b.get();
}

namespace {

// Integral of the magnification wave (a raised cosine over [-1, 1]) from 0 to x.
float WaveArea(float x)
{
    constexpr float pi = 3.14159265f;
    if (x >= 1)
        return 0.5f;
    if (x <= -1)
        return -0.5f;
    return x / 2 + std::sin(pi * x) / (2 * pi);
}

} // namespace

// Displayed position of base position u. The local scale is 1 + (mag-1) *
// amount * wave((t - centre) / range); integrating it from the pointer keeps
// the point under the pointer fixed, so the icon under the pointer in the base
// layout is the one shown under it, and the neighbours spread out like a wave.
float BarPainter::MapX(float u) const
{
    float k = (mag_ - 1) * amount_;
    if (k <= 0)
        return u;
    return u + k * range_ * WaveArea((u - centre_) / range_);
}

bool BarPainter::Animate(const BarScene& scene, double nowMs)
{
    double dt = lastMs_ == 0 ? 16 : std::clamp(nowMs - lastMs_, 0.0, 100.0);
    lastMs_ = nowMs;
    bool moving = false;
    // A new window geometry: items jump to their places instead of sliding
    // across from coordinates of the old window. (The window keeps its width
    // when apps come and go; then the bar's edges and the items slide.)
    bool snap = scene.windowW != lastWindowW_ || scene.bar.top != lastTop_ || scene.bar.bottom != lastBottom_;
    lastWindowW_ = scene.windowW;
    lastTop_ = scene.bar.top;
    lastBottom_ = scene.bar.bottom;
    auto slide = [&](float& v, float target) {
        if (snap) {
            v = target;
            return;
        }
        float n = ApproachPx(v, target, dt, kSlideRate);
        moving |= n != v;
        v = n;
    };
    slide(barL_, scene.bar.left);
    slide(barR_, scene.bar.right);
    if (scene.separatorX < 0 || sepX_ < 0)
        sepX_ = scene.separatorX;
    else
        slide(sepX_, scene.separatorX);
    slide(clockX_, (scene.clock.left + scene.clock.right) / 2);

    // Magnification: follows the pointer at once, grows and settles smoothly.
    bool hovering = scene.pointerX >= 0 && scene.dragKey.empty() && scene.magnify > 1.001f;
    mag_ = std::max(1.f, scene.magnify);
    range_ = std::max(1.f, scene.magnifyRange);
    if (hovering)
        centre_ = scene.pointerX;
    float amount = Approach(amount_, hovering ? 1.f : 0.f, dt, 13);
    moving |= amount != amount_;
    amount_ = amount;

    for (auto& [key, m] : motion_)
        m.inScene = false;
    auto step = [&](const std::wstring& key, const SceneItem* item) {
        Motion& m = motion_[key];
        m.inScene = true;
        float h = Approach(m.hover, scene.hoverKey == key || scene.dragKey == key ? 1.f : 0.f, dt, 18);
        float p = Approach(m.press, scene.pressedKey == key && scene.dragKey != key ? 1.f : 0.f, dt, 30);
        float a = Approach(m.appear, 1.f, dt, 10);
        float l = Approach(m.lift, scene.dragKey == key ? 1.f : 0.f, dt, 22);
        float d = Approach(m.dot, item && item->running ? 1.f : 0.f, dt, 12);
        moving |= h != m.hover || p != m.press || a != m.appear || l != m.lift || d != m.dot;
        m.hover = h;
        m.press = p;
        m.appear = a;
        m.lift = l;
        m.dot = d;
        m.leaving = false; // back again
        if (!item)
            return;
        float x = (item->plate.left + item->plate.right) / 2;
        if (!m.placed || snap || scene.dragKey == key) {
            m.x = x;
            m.placed = true;
        } else {
            float nx = ApproachPx(m.x, x, dt, kSlideRate);
            moving |= nx != m.x;
            m.x = nx;
        }
        // Launch: one soft hop (v0.9.8; it was up to four, each lower). It leaves and lands
        // without a jolt (sin², no speed at either end), whatever the app does meanwhile.
        m.hop = 0;
        if (item->bounceMs > 0) {
            constexpr double kHopMs = 480;
            double t = nowMs - item->bounceMs;
            if (t >= 0 && t < kHopMs) {
                float sn = std::sin(3.14159265f * float(t / kHopMs));
                m.hop = sn * sn;
                moving = true;
            }
        }
    };
    for (const auto& it : scene.items)
        step(it.key, &it);
    for (const auto& it : scene.extras)
        step(it.key, &it);
    step(L"#clock", nullptr);
    // Apps that went away shrink and fade out where they were while the
    // others close up; other entries just go.
    for (auto it = motion_.begin(); it != motion_.end();) {
        Motion& m = it->second;
        if (m.inScene) {
            ++it;
            continue;
        }
        auto last = lastApps_.find(it->first);
        if (!m.leaving && last != lastApps_.end() && !snap) {
            m.leaving = true;
            m.ghost = last->second;
            m.hover = m.press = m.lift = 0;
            m.hop = 0;
        }
        lastApps_.erase(it->first);
        if (m.leaving) {
            m.appear = Approach(m.appear, 0.f, dt, 11);
            m.dot = Approach(m.dot, 0.f, dt, 14);
        }
        if (!m.leaving || m.appear < 0.02f || snap) {
            it = motion_.erase(it);
        } else {
            moving = true;
            ++it;
        }
    }
    for (const auto& it : scene.items)
        lastApps_[it.key] = it;

    // Status ring: a changed volume or brightness shows on the ring for a
    // while; the fill follows on a spring with a slight overshoot (no linear
    // sweep).
    {
        constexpr double kLevelShowMs = 1500;
        const SystemStatus& st = scene.status;
        StatusMotion& s = statusMotion_;
        if (s.seen && st.audio && (st.volume != s.volume || st.muted != s.muted)) {
            s.volumeUntilMs = nowMs + kLevelShowMs;
            s.brightUntilMs = 0;
        }
        // The first reported brightness is the current one, not a change.
        if (s.seen && st.brightness >= 0 && s.brightness >= 0 && st.brightness != s.brightness) {
            s.brightUntilMs = nowMs + kLevelShowMs;
            s.volumeUntilMs = 0;
        }
        s.volume = st.volume;
        s.muted = st.muted;
        s.brightness = st.brightness;
        bool showVolume = st.audio && nowMs < s.volumeUntilMs, showBright = st.brightness >= 0 && nowMs < s.brightUntilMs;
        moving |= showVolume || showBright;
        float mix = Approach(s.volumeMix, showVolume ? 1.f : 0.f, dt, 12), bmix = Approach(s.brightMix, showBright ? 1.f : 0.f, dt, 12);
        moving |= mix != s.volumeMix || bmix != s.brightMix;
        s.volumeMix = mix;
        s.brightMix = bmix;
        float volume = st.muted ? 0.f : float(st.volume) / 100.f;
        float target = showBright ? float(st.brightness) / 100.f
                       : (showVolume || !st.battery) ? volume
                                                     : float(std::clamp(st.batteryPercent, 0, 100)) / 100.f;
        if (!s.seen) {
            s.fill = target;
            s.seen = true;
        }
        motion::kSmooth.Step(s.fill, s.speed, target, float(dt) / 1000.f); // about 0.4 s, under 5 % overshoot
        if (motion::Spring::AtRest(s.fill, s.speed, target)) {
            s.fill = target;
            s.speed = 0;
        } else {
            moving = true;
        }
    }

    // Auto-hide slide.
    if (scene.tucked != slideDown_) {
        slideDown_ = scene.tucked;
        slideFrom_ = slide_;
        slideStartMs_ = nowMs;
        Log(L"bar: slide %s", slideDown_ ? L"down" : L"up");
    }
    {
        const float target = slideDown_ ? 1.f : 0.f;
        if (slide_ != target) {
            double length = slideDown_ ? kSlideOutMs : kSlideInMs;
            float u = float(std::clamp((nowMs - slideStartMs_) / length, 0.0, 1.0));
            float e;
            if (slideDown_) {
                e = 0.5f - 0.5f * std::cos(3.14159265f * u); // eases in and out
            } else {
                e = motion::EaseOutBack(u, 1.2f); // a slight overshoot
            }
            slide_ = u >= 1 ? target : slideFrom_ + (target - slideFrom_) * e;
            moving = true;
        }
    }
    frame_.slideY = slide_ * std::max(0.f, scene.windowH - scene.bar.top);

    // What the glass shows this frame: the stretched shape and the ripple.
    frame_.shape = D2D1::RectF(MapX(barL_), scene.bar.top + frame_.slideY, MapX(barR_), scene.bar.bottom + frame_.slideY);
    // Share the icons' wave and easing; its support widens with the stretched bar.
    frame_.waveX = centre_;
    frame_.waveWidth = range_ * (1 + (mag_ - 1) * amount_ / 2);
    frame_.waveHeight = scene.magnifyLift * amount_;
    frame_.rippleA = 0;
    if (scene.rippleMs > 0) {
        constexpr double kRippleMs = 720;
        double t = nowMs - scene.rippleMs;
        if (t >= 0 && t < kRippleMs) {
            float u = float(t / kRippleMs);
            frame_.rippleX = MapX(scene.rippleX);
            frame_.rippleY = scene.rippleY + frame_.slideY;
            frame_.rippleR = (scene.bar.bottom - scene.bar.top) * 1.5f * motion::EaseOut(u);
            frame_.rippleA = 0.9f * (1 - u) * (1 - u);
            moving = true;
        }
    }
    return moving;
}

D2D1_ELLIPSE BarPainter::RunningDot(const BarScene& scene, const SceneItem& item, const Motion& motion) const
{
    float dx = motion.placed ? motion.x - (item.plate.left + item.plate.right) / 2 : 0;
    float x = MapX((item.icon.left + item.icon.right) / 2 + dx);
    float y = (item.icon.bottom + scene.bar.bottom) / 2;
    float radius = (item.active ? 2.3f : 1.9f) * scene.scale;
    return D2D1::Ellipse(D2D1::Point2F(x, y), radius, radius);
}

bool BarPainter::UpdateDotInk(const BarScene& scene, const Readback& backdrop, const Settings& settings, double nowMs)
{
    float mix = float(1 - settings.transparency);
    float tint = (0.2126f * float((settings.tint >> 16) & 255) + 0.7152f * float((settings.tint >> 8) & 255) +
                  0.0722f * float(settings.tint & 255)) / 255.f;
    bool moving = false;
    for (const auto& item : scene.items) {
        auto& m = motion_[item.key];
        if (!item.running && m.dot < 0.01f)
            continue;
        if (!backdrop.luma.empty()) {
            auto dot = RunningDot(scene, item, m);
            float x = dot.point.x + GlassRenderer::kPad;
            float y = dot.point.y + std::round(frame_.slideY) + GlassRenderer::kPad;
            float luma = backdrop.RegionLuma(x - dot.radiusX, y - dot.radiusY, x + dot.radiusX, y + dot.radiusY) * (1 - mix) + tint * mix;
            if (!m.darkDot && luma > 0.50f)
                m.darkDot = true;
            else if (m.darkDot && luma < 0.42f)
                m.darkDot = false;
        }
        bool dark = settings.textColor == L"dark" || (settings.textColor == L"auto" && m.darkDot);
        float before = m.dotInk.Value(nowMs);
        m.dotInk.Target(dark ? 1.f : 0.f, nowMs);
        moving |= before != m.dotInk.Value(nowMs) || m.dotInk.Moving(nowMs);
    }
    return moving;
}

void BarPainter::Draw(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink ink, const std::wstring& time, const std::wstring& date,
                      const std::wstring& contentKey, bool animating)
{
    winrt::com_ptr<ID2D1Image> target;
    d2d->GetTarget(target.put());
    auto size = d2d->GetPixelSize();
    if (layer_ && (layer_->GetPixelSize().width != size.width || layer_->GetPixelSize().height != size.height))
        layer_ = nullptr;
    if (!layer_) {
        D2D1_BITMAP_PROPERTIES1 bp =
            D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        if (FAILED(d2d->CreateBitmap(size, nullptr, 0, &bp, layer_.put()))) {
            DrawContent(d2d, scene, ink, time, date); // no layer: draw directly
            return;
        }
        layerKey_.clear();
    }
    if (animating || contentKey != layerKey_) {
        d2d->SetTarget(layer_.get());
        d2d->Clear(D2D1::ColorF(0, 0, 0, 0));
        DrawContent(d2d, scene, ink, time, date);
        d2d->SetTarget(target.get());
        layerKey_ = animating ? std::wstring() : contentKey;
    }
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());
    const D2D1_POINT_2F at = D2D1::Point2F(0, std::round(frame_.slideY));
    d2d->DrawImage(layer_.get(), &at, nullptr, D2D1_INTERPOLATION_MODE_NEAREST_NEIGHBOR, D2D1_COMPOSITE_MODE_SOURCE_OVER);
}

void BarPainter::DrawContent(ID2D1DeviceContext2* d2d, const BarScene& scene, Ink inks, const std::wstring& time, const std::wstring& date)
{
    const float s = scene.scale;
    // Ink colours: white on dark glass, near-black on light glass, per region.
    // `dark` is the amount of dark ink: colours in between while the ink fades.
    auto mix = [](float dark, D2D1_COLOR_F d, D2D1_COLOR_F l) {
        return D2D1::ColorF(l.r + (d.r - l.r) * dark, l.g + (d.g - l.g) * dark, l.b + (d.b - l.b) * dark, l.a + (d.a - l.a) * dark);
    };
    auto inkFor = InkColor;
    auto haloFor = [&](float dark) { return mix(dark, D2D1::ColorF(1, 1, 1, 0.35f), D2D1::ColorF(0, 0, 0, 0.32f)); };
    auto washFor = [&](float dark) { return mix(dark, D2D1::ColorF(0, 0, 0, 0.10f), D2D1::ColorF(1, 1, 1, 0.20f)); };
    float darkText = inks.darkClock;
    D2D1_COLOR_F ink = inkFor(inks.darkApps);
    D2D1_COLOR_F halo = haloFor(darkText);
    D2D1_COLOR_F wash = washFor(inks.darkApps);
    const auto& brush = contentBrush_;
    brush->SetColor(ink);
    d2d->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    d2d->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // premultiplied target: no ClearType

    usedBitmaps_.clear();
    for (const auto& it : scene.extras) { // the right group follows the clock's ink, the others the apps'
        bool right = it.key == L"#notify" || it.key == L"#tray" || it.key == L"#status";
        DrawItem(d2d, scene, it, brush.get(), right ? inkFor(darkText) : ink, right ? washFor(darkText) : wash);
    }
    for (const auto& [key, m] : motion_)
        if (m.leaving)
            DrawItem(d2d, scene, m.ghost, brush.get(), ink, wash);
    for (const auto& it : scene.items)
        if (it.key != scene.dragKey)
            DrawItem(d2d, scene, it, brush.get(), ink, wash);
    for (const auto& it : scene.items)
        if (it.key == scene.dragKey)
            DrawItem(d2d, scene, it, brush.get(), ink, wash); // on top of the others
    // Drop bitmaps of icons that are gone or were replaced.
    for (auto it = bitmaps_.begin(); it != bitmaps_.end();)
        it = std::find(usedBitmaps_.begin(), usedBitmaps_.end(), it->first) != usedBitmaps_.end() ? std::next(it) : bitmaps_.erase(it);

    if (sepX_ >= 0) {
        D2D1_COLOR_F c = inkFor(darkText);
        c.a = 0.30f + (0.18f - 0.30f) * darkText;
        brush->SetColor(c);
        float h = (scene.bar.bottom - scene.bar.top) * 0.46f, mid = (scene.bar.top + scene.bar.bottom) / 2, x = MapX(sepX_);
        d2d->FillRectangle(D2D1::RectF(x - 0.5f * s, mid - h / 2, x + 0.5f * s, mid + h / 2), brush.get());
    }

    if (scene.clock.right > scene.clock.left) {
        const Motion& m = motion_[L"#clock"];
        ink = inkFor(darkText);
        // The clock does not grow; it moves with the stretched bar.
        float shift = MapX(clockX_) - (scene.clock.left + scene.clock.right) / 2;
        d2d->SetTransform(D2D1::Matrix3x2F::Translation(shift, 0));
        if (m.hover > 0.001f) {
            D2D1_COLOR_F c = washFor(darkText);
            c.a *= motion::EaseOut(m.hover);
            brush->SetColor(c);
            float r = scene.plateRadius;
            FillContinuousRect(d2d, scene.clock, r, brush.get());
        }
        float lineT = scene.timePx * 1.22f, lineD = scene.datePx * 1.25f;
        float top = (scene.clock.top + scene.clock.bottom) / 2 - (lineT + lineD) / 2;
        auto text = [&](const std::wstring& str, IDWriteTextFormat* f, float y0, float y1) {
            if (!f)
                return;
            float o = std::max(1.0f, s * 0.6f);
            brush->SetColor(halo);
            DrawUiText(d2d, dwrite_.get(), str, f, D2D1::RectF(scene.clock.left, y0 + o, scene.clock.right, y1 + o), brush.get());
            brush->SetColor(ink);
            DrawUiText(d2d, dwrite_.get(), str, f, D2D1::RectF(scene.clock.left, y0, scene.clock.right, y1), brush.get());
        };
        text(time, Format(scene.timePx, DWRITE_FONT_WEIGHT_SEMI_BOLD), top, top + lineT);
        text(date, Format(scene.datePx, DWRITE_FONT_WEIGHT_NORMAL), top + lineT, top + lineT + lineD);
        d2d->SetTransform(D2D1::Matrix3x2F::Identity());
    }
}

void BarPainter::DrawItem(ID2D1DeviceContext2* d2d, const BarScene& scene, const SceneItem& it, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink,
                          D2D1_COLOR_F wash)
{
    const float s = scene.scale;
    const Motion& m = motion_[it.key];
    const bool magnifying = mag_ > 1.001f;
    // Base position: everything of the item slides with its plate (reordering,
    // drag and drop). Displayed position: the base one under the magnification.
    float dx = m.placed ? m.x - (it.plate.left + it.plate.right) / 2 : 0;
    D2D1_RECT_F plate = D2D1::RectF(it.plate.left + dx, it.plate.top, it.plate.right + dx, it.plate.bottom);
    D2D1_RECT_F icon = D2D1::RectF(it.icon.left + dx, it.icon.top, it.icon.right + dx, it.icon.bottom);
    float size = icon.right - icon.left;
    float bx = (icon.left + icon.right) / 2, bottom = icon.bottom;
    float pw = std::max(1.f, plate.right - plate.left);
    float grow = (MapX(bx + pw / 2) - MapX(bx - pw / 2)) / pw; // local magnification
    float cx = MapX(bx);
    float hover = motion::EaseOut(m.hover), lift = motion::EaseOut(m.lift);

    // 底板仅响应悬停（关闭放大时）和拖动，按下仅缩放图标。
    // Its corners are concentric with the bar's.
    float plateA = (magnifying ? 0.f : hover) + 0.4f * lift;
    if (plateA > 0.001f) {
        D2D1_COLOR_F c = wash;
        c.a *= std::min(plateA, 1.6f);
        brush->SetColor(c);
        float r = scene.plateRadius;
        d2d->SetTransform(D2D1::Matrix3x2F::Scale(grow, grow, D2D1::Point2F(bx, plate.bottom)) * D2D1::Matrix3x2F::Translation(cx - bx, 0));
        FillContinuousRect(d2d, plate, r, brush);
    }

    // Icon: grows from its bottom edge with the wave, hops when launching,
    // dips on press, fades in when new; lifts while dragged.
    float k = grow * (magnifying ? 1.f : 1 + 0.06f * hover) * (1 + 0.08f * lift) * (1 - 0.08f * m.press) * (0.85f + 0.15f * m.appear) *
              (m.leaving ? 0.45f + 0.55f * m.appear : 1.f);
    float up = m.hop * 0.32f * size + (magnifying ? 0.f : 1.5f * s * hover) + 2.0f * s * lift;
    d2d->SetTransform(D2D1::Matrix3x2F::Scale(k, k, D2D1::Point2F(bx, bottom)) * D2D1::Matrix3x2F::Translation(cx - bx, -up));
    if (it.key == L"#status") {
        bool darkInk = ink.r < 0.5f;
        if (scene.statusIcons)
            DrawStatusIcons(d2d, scene.status, icon, brush, ink, darkInk);
        else
            DrawStatus(d2d, scene.status, icon, brush, ink, darkInk);
        d2d->SetTransform(D2D1::Matrix3x2F::Identity());
        return;
    }
    if (it.key == L"#start") {
        DrawLogo(d2d, icon, m.appear);
    } else if (!it.glyph.empty()) {
        if (IDWriteTextFormat* f = Format(size * 0.62f, DWRITE_FONT_WEIGHT_NORMAL, L"Segoe Fluent Icons")) {
            brush->SetColor(ink);
            d2d->DrawText(it.glyph.c_str(), UINT32(it.glyph.size()), f, icon, brush);
        }
    } else if (ID2D1Bitmap1* bmp = BitmapFor(d2d, it)) {
        usedBitmaps_.push_back(it.imageId);
        shadow_->SetInput(0, bmp);
        shadow_->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, (1.6f + 1.6f * lift + 1.2f * m.hop) * s);
        shadow_->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0, 0, 0, (0.38f + 0.12f * lift) * m.appear));
        auto px = bmp->GetPixelSize();
        float sx = size / float(px.width), sy = size / float(px.height);
        D2D1_MATRIX_3X2_F t;
        d2d->GetTransform(&t);
        d2d->SetTransform(D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Translation(icon.left, icon.top + (1.2f + 1.5f * lift) * s) * t);
        d2d->DrawImage(shadow_.get(), D2D1_INTERPOLATION_MODE_LINEAR);
        d2d->SetTransform(t);
        d2d->DrawBitmap(bmp, icon, m.appear, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
    } else {
        // Placeholder while the icon loads: a quiet rounded square.
        D2D1_COLOR_F c = wash;
        c.a *= 1.4f * m.appear;
        brush->SetColor(c);
        FillContinuousRect(d2d, icon, size * 0.22f, brush);
    }
    // Count badge (unread notifications): a small accent disc on the icon's corner.
    if (it.badge > 0) {
        float d = std::max(9.f * s, size * 0.46f);
        float bxr = icon.right - d * 0.35f, by = icon.top + d * 0.15f;
        brush->SetColor(D2D1::ColorF(0.04f, 0.52f, 1.0f, 1.0f));
        d2d->FillEllipse(D2D1::Ellipse(D2D1::Point2F(bxr, by), d / 2, d / 2), brush);
        std::wstring n = it.badge > 9 ? L"9+" : std::to_wstring(it.badge);
        if (IDWriteTextFormat* f = Format(d * 0.62f, DWRITE_FONT_WEIGHT_SEMI_BOLD)) {
            brush->SetColor(D2D1::ColorF(1, 1, 1, 1));
            DrawUiText(d2d, dwrite_.get(), n, f, D2D1::RectF(bxr - d, by - d / 2, bxr + d, by + d / 2), brush);
        }
    }
    d2d->SetTransform(D2D1::Matrix3x2F::Identity());

    // Running indicator: a small dot under the icon, centred in the gap to the
    // bar's lower edge; full ink for the foreground app, amber when the app
    // asks for attention. It stays on the bar while the icon hops or grows.
    if (m.dot > 0.01f && lift < 0.99f) {
        D2D1_COLOR_F c = it.attention ? D2D1::ColorF(1.0f, 0.70f, 0.18f, 0.98f) : InkColor(m.dotInk.Value(lastMs_));
        if (!it.active && !it.attention)
            c.a *= 0.45f;
        c.a *= (1 - lift) * m.appear * m.dot;
        brush->SetColor(c);
        d2d->FillEllipse(RunningDot(scene, it, m), brush);
    }
}

// The status icon: the connection alone, in the bar's ink like every other
// symbol on it. Wi-Fi is a fan of three arcs over a dot, as many lit as the
// signal is strong; a plug when wired; the fan dim when offline. The battery
// and the volume are in the control centre.
void BarPainter::DrawStatus(ID2D1DeviceContext2* d2d, const SystemStatus& st, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink,
                            bool darkInk)
{
    const float h = std::min(r.right - r.left, r.bottom - r.top), cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    const StatusMotion& sm = statusMotion_;
    auto dim = [&](float a) {
        D2D1_COLOR_F c = ink;
        c.a *= a;
        return c;
    };
    auto fade = [](D2D1_COLOR_F c, float a) {
        c.a *= a;
        return c;
    };
    winrt::com_ptr<ID2D1Factory> factory;
    d2d->GetFactory(factory.put());
    ID2D1StrokeStyle* round = RoundStroke(d2d);
    constexpr float kDeg = 3.14159265f / 180.f;
    // An arc from `a0` clockwise over `sweep` degrees (0° = right, 90° = down).
    auto arc = [&](float ox, float oy, float rad, float a0, float sweep, D2D1_COLOR_F c, float width, StatusArc& cached) {
        if (sweep <= 0.01f || c.a <= 0.003f)
            return;
        if (!cached.path || cached.sweep != sweep) {
            cached.path = nullptr;
            winrt::com_ptr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(cached.path.put())) || FAILED(cached.path->Open(sink.put())))
                return;
            float a1 = a0 + sweep;
            sink->BeginFigure(D2D1::Point2F(std::cos(a0 * kDeg), std::sin(a0 * kDeg)), D2D1_FIGURE_BEGIN_HOLLOW);
            sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(std::cos(a1 * kDeg), std::sin(a1 * kDeg)), D2D1::SizeF(1, 1), 0,
                                          D2D1_SWEEP_DIRECTION_CLOCKWISE, sweep > 180 ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
            sink->Close();
            cached.sweep = sweep;
        }
        brush->SetColor(c);
        D2D1_MATRIX_3X2_F previous;
        d2d->GetTransform(&previous);
        d2d->SetTransform(D2D1::Matrix3x2F::Scale(rad, rad) * D2D1::Matrix3x2F::Translation(ox, oy) * previous);
        d2d->DrawGeometry(cached.path.get(), brush, width / rad, round);
        d2d->SetTransform(previous);
    };
    // The connection: the Wi-Fi fan (a dot and three arcs, lit by the signal),
    // the wired glyph, or a faint fan when offline. `size` is the fan's height.
    auto connection = [&](float size, float alpha) {
        if (alpha <= 0.003f)
            return;
        if (st.connected && !st.wifi) {
            if (IDWriteTextFormat* f = Format(size * 0.62f, DWRITE_FONT_WEIGHT_NORMAL, L"Segoe Fluent Icons")) {
                brush->SetColor(fade(ink, alpha));
                d2d->DrawText(L"\uE839", 1, f, D2D1::RectF(cx - size, cy - size / 2, cx + size, cy + size / 2), brush);
            }
            return;
        }
        // The iPhone's fan (wifi_glyph); `size` is the room it has.
        const int lit = !st.connected ? -1 : WifiSteps(st.signal); // -1: offline
        DrawWifiFan(d2d, brush, cx, cy, size * 0.62f, lit, fade(ink, alpha), dim((lit < 0 ? 0.32f : 0.24f) * alpha));
    };

    const bool ring = st.battery || st.audio || sm.brightMix > 0.003f;
    if (!ring) { // nothing to measure: the fan alone, full size
        connection(h, 1);
        return;
    }
    // The ring: open at the bottom like the iPhone's, filled clockwise from
    // the gap's left end. Ink like every mark on the bar; only a low battery
    // (not charging) is red.
    constexpr float kGap = 64, kStart = 90 + kGap / 2, kSweep = 360 - kGap;
    const float rad = h * 0.44f, stroke = std::max(1.5f, h * 0.07f);
    // How much the ring shows the volume or the brightness rather than the battery.
    const float shown = std::min(1.f, sm.volumeMix + sm.brightMix);
    const float mix = st.battery ? sm.volumeMix : 1.f - sm.brightMix; // 1: the ring shows the volume
    const bool low = st.battery && !st.charging && st.batteryPercent >= 0 && st.batteryPercent <= 20;
    D2D1_COLOR_F level = ink;
    if (low) {
        D2D1_COLOR_F red = D2D1::ColorF(darkInk ? 0xE0352B : 0xFF453A);
        float k = 1 - shown;
        level = D2D1::ColorF(red.r + (ink.r - red.r) * (1 - k), red.g + (ink.g - red.g) * (1 - k), red.b + (ink.b - red.b) * (1 - k), ink.a);
    }
    const bool silent = st.muted && mix > 0.5f;
    arc(cx, cy, rad, kStart, kSweep, dim(silent ? 0.16f : 0.24f), stroke, ringTrack_);
    arc(cx, cy, rad, kStart, kSweep * std::clamp(sm.fill, 0.f, 1.f), level, stroke, ringLevel_);

    // In the gap: a bolt while charging (fades out while a level shows).
    float bolt = st.battery && st.charging ? 1 - shown : 0;
    if (bolt > 0.003f) {
        if (!chargingBolt_) {
            winrt::com_ptr<ID2D1GeometrySink> sink;
            if (FAILED(factory->CreatePathGeometry(chargingBolt_.put())) || FAILED(chargingBolt_->Open(sink.put())))
                return;
            const D2D1_POINT_2F pts[] = {{0.18f, -0.5f}, {-0.4f, 0.08f}, {-0.02f, 0.08f}, {-0.18f, 0.5f}, {0.4f, -0.08f}, {0.02f, -0.08f}};
            auto at = [](D2D1_POINT_2F p) { return D2D1::Point2F(p.x * 0.17f, 0.44f + p.y * 0.26f); };
            sink->BeginFigure(at(pts[0]), D2D1_FIGURE_BEGIN_FILLED);
            for (int i = 1; i < 6; ++i)
                sink->AddLine(at(pts[i]));
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
        }
        D2D1_MATRIX_3X2_F previous;
        d2d->GetTransform(&previous);
        d2d->SetTransform(D2D1::Matrix3x2F::Scale(h, h) * D2D1::Matrix3x2F::Translation(cx, cy) * previous);
        brush->SetColor(fade(ink, bolt));
        d2d->FillGeometry(chargingBolt_.get(), brush);
        d2d->SetTransform(previous);
    }

    // Inside: the connection, or the speaker or the sun while a level shows.
    const float speaker = st.battery ? sm.volumeMix : 0.f;
    connection(h * 0.6f, 1 - std::min(1.f, speaker + sm.brightMix));
    auto symbol = [&](const wchar_t* glyph, float alpha) {
        if (alpha <= 0.003f)
            return;
        if (IDWriteTextFormat* f = Format(h * 0.40f, DWRITE_FONT_WEIGHT_NORMAL, L"Segoe Fluent Icons")) {
            brush->SetColor(fade(ink, alpha));
            d2d->DrawText(glyph, 1, f, D2D1::RectF(cx - h / 2, cy - h / 2, cx + h / 2, cy + h / 2), brush);
        }
    };
    const wchar_t* volumeGlyph = st.muted || st.volume == 0 ? L"\uE74F" : st.volume < 34 ? L"\uE993" : st.volume < 67 ? L"\uE994" : L"\uE995";
    symbol(volumeGlyph, speaker);
    symbol(L"\uE706", sm.brightMix);

    // No battery (a desktop): the ring is the volume, so its gap holds a small speaker where a
    // laptop's bolt would be (gone while the brightness shows).
    const float gapSpeaker = !st.battery && st.audio ? 1 - sm.brightMix : 0.f;
    if (gapSpeaker > 0.003f)
        if (IDWriteTextFormat* f = Format(h * 0.22f, DWRITE_FONT_WEIGHT_NORMAL, L"Segoe Fluent Icons")) {
            brush->SetColor(fade(ink, gapSpeaker));
            const float gy = cy + rad;
            d2d->DrawText(volumeGlyph, 1, f, D2D1::RectF(cx - h / 4, gy - h / 4, cx + h / 4, gy + h / 4), brush);
        }
}

void BarPainter::DrawStatusIcons(ID2D1DeviceContext2* d2d, const SystemStatus& st, D2D1_RECT_F r, ID2D1SolidColorBrush* brush, D2D1_COLOR_F ink,
                                 bool darkInk)
{
    const float h = r.bottom - r.top, cy = (r.top + r.bottom) / 2;
    auto fade = [](D2D1_COLOR_F c, float a) {
        c.a *= a;
        return c;
    };
    winrt::com_ptr<ID2D1Factory> factory;
    d2d->GetFactory(factory.put());
    ID2D1StrokeStyle* round = RoundStroke(d2d);
    const float stroke = std::max(1.f, h * 0.075f);
    // Widths of the three (in icon heights) and the room between them.
    const float netW = 0.62f * h, speakerW = 0.60f * h, batteryW = 0.68f * h, gap = 0.18f * h;
    float x = (r.left + r.right) / 2 - (netW + speakerW + (st.battery ? batteryW + gap : 0) + (st.audio ? 0 : -speakerW - gap) + gap) / 2;

    // The network: the Wi-Fi fan lit by the signal, the wired glyph, or a faint fan offline.
    if (st.connected && !st.wifi) {
        if (IDWriteTextFormat* f = Format(h * 0.62f, DWRITE_FONT_WEIGHT_NORMAL, L"Segoe Fluent Icons")) {
            brush->SetColor(ink);
            d2d->DrawText(L"\uE839", 1, f, D2D1::RectF(x - h, cy - h / 2, x + netW + h, cy + h / 2), brush);
        }
    } else {
        const int lit = !st.connected ? -1 : WifiSteps(st.signal);
        DrawWifiFan(d2d, brush, x + netW / 2, cy, h * 0.46f, lit, ink, fade(ink, lit < 0 ? 0.32f : 0.24f));
    }
    x += netW + gap;

    // The speaker: a cone, and a wave for each third of the volume (the others faint); muted, a cross.
    if (st.audio) {
        const float l = x, mid = cy;
        winrt::com_ptr<ID2D1PathGeometry> cone;
        winrt::com_ptr<ID2D1GeometrySink> sink;
        if (SUCCEEDED(factory->CreatePathGeometry(cone.put())) && SUCCEEDED(cone->Open(sink.put()))) {
            const D2D1_POINT_2F pts[] = {{l, mid - 0.10f * h},          {l + 0.11f * h, mid - 0.10f * h}, {l + 0.25f * h, mid - 0.22f * h},
                                         {l + 0.25f * h, mid + 0.22f * h}, {l + 0.11f * h, mid + 0.10f * h}, {l, mid + 0.10f * h}};
            sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
            for (int i = 1; i < 6; ++i)
                sink->AddLine(pts[i]);
            sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->Close();
            brush->SetColor(fade(ink, st.muted ? 0.55f : 1.f));
            d2d->FillGeometry(cone.get(), brush);
            d2d->DrawGeometry(cone.get(), brush, stroke * 0.6f, round);
        }
        const float ox = l + 0.25f * h;
        if (st.muted) {
            const float c = ox + 0.19f * h, k = 0.08f * h;
            brush->SetColor(ink);
            d2d->DrawLine(D2D1::Point2F(c - k, mid - k), D2D1::Point2F(c + k, mid + k), brush, stroke, round);
            d2d->DrawLine(D2D1::Point2F(c - k, mid + k), D2D1::Point2F(c + k, mid - k), brush, stroke, round);
        } else {
            const int waves = st.volume <= 0 ? 0 : st.volume < 34 ? 1 : st.volume < 67 ? 2 : 3;
            for (int i = 0; i < 3; ++i) {
                const float rad = (0.11f + 0.105f * float(i)) * h, a = 0.75f;
                winrt::com_ptr<ID2D1PathGeometry> arc;
                winrt::com_ptr<ID2D1GeometrySink> s2;
                if (FAILED(factory->CreatePathGeometry(arc.put())) || FAILED(arc->Open(s2.put())))
                    break;
                s2->BeginFigure(D2D1::Point2F(ox + rad * std::cos(-a), mid + rad * std::sin(-a)), D2D1_FIGURE_BEGIN_HOLLOW);
                s2->AddArc(D2D1::ArcSegment(D2D1::Point2F(ox + rad * std::cos(a), mid + rad * std::sin(a)), D2D1::SizeF(rad, rad), 0,
                                            D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
                s2->EndFigure(D2D1_FIGURE_END_OPEN);
                s2->Close();
                brush->SetColor(i < waves ? ink : fade(ink, 0.24f));
                d2d->DrawGeometry(arc.get(), brush, stroke, round);
            }
        }
        x += speakerW + gap;
    }

    // The battery: an outline with its level inside (red when low and not charging), a bolt cut
    // out of the level while charging.
    if (st.battery) {
        const float bw = 0.58f * h, bh = 0.36f * h, rad = 0.09f * h;
        const D2D1_RECT_F body = D2D1::RectF(x + stroke / 2, cy - bh / 2, x + bw, cy + bh / 2);
        brush->SetColor(fade(ink, 0.9f));
        FillContinuousRect(d2d, D2D1::RectF(body.right + 0.03f * h, cy - 0.075f * h, body.right + 0.08f * h, cy + 0.075f * h), 0.02f * h, brush);
        d2d->DrawRoundedRectangle(D2D1::RoundedRect(body, rad, rad), brush, stroke);
        const float pad = stroke + 0.035f * h, level = std::clamp(float(st.batteryPercent) / 100.f, 0.f, 1.f);
        const D2D1_RECT_F inner = D2D1::RectF(body.left + pad, body.top + pad, body.right - pad, body.bottom - pad);
        const bool low = !st.charging && st.batteryPercent >= 0 && st.batteryPercent <= 20;
        D2D1_COLOR_F fill = low ? D2D1::ColorF(darkInk ? 0xE0352B : 0xFF453A) : ink;
        D2D1_RECT_F lit = inner;
        lit.right = inner.left + std::max(0.06f * h, (inner.right - inner.left) * level);
        winrt::com_ptr<ID2D1RoundedRectangleGeometry> levelShape;
        factory->CreateRoundedRectangleGeometry(D2D1::RoundedRect(lit, 0.035f * h, 0.035f * h), levelShape.put());
        winrt::com_ptr<ID2D1PathGeometry> bolt;
        if (st.charging) {
            winrt::com_ptr<ID2D1GeometrySink> s3;
            if (SUCCEEDED(factory->CreatePathGeometry(bolt.put())) && SUCCEEDED(bolt->Open(s3.put()))) {
                const float bx = (body.left + body.right) / 2, k = bh * 0.62f;
                const D2D1_POINT_2F pts[] = {{0.18f, -0.5f}, {-0.4f, 0.08f}, {-0.02f, 0.08f}, {-0.18f, 0.5f}, {0.4f, -0.08f}, {0.02f, -0.08f}};
                s3->BeginFigure(D2D1::Point2F(bx + pts[0].x * k, cy + pts[0].y * k * 1.5f), D2D1_FIGURE_BEGIN_FILLED);
                for (int i = 1; i < 6; ++i)
                    s3->AddLine(D2D1::Point2F(bx + pts[i].x * k, cy + pts[i].y * k * 1.5f));
                s3->EndFigure(D2D1_FIGURE_END_CLOSED);
                s3->Close();
            } else {
                bolt = nullptr;
            }
        }
        brush->SetColor(fill);
        if (bolt && levelShape) {
            // The level less the bolt, and the bolt where there is no level.
            auto combine = [&](ID2D1Geometry* a, ID2D1Geometry* b) {
                winrt::com_ptr<ID2D1PathGeometry> out;
                winrt::com_ptr<ID2D1GeometrySink> s4;
                if (FAILED(factory->CreatePathGeometry(out.put())) || FAILED(out->Open(s4.put())))
                    return winrt::com_ptr<ID2D1PathGeometry>{};
                a->CombineWithGeometry(b, D2D1_COMBINE_MODE_EXCLUDE, nullptr, s4.get());
                s4->Close();
                return out;
            };
            if (auto cut = combine(levelShape.get(), bolt.get()))
                d2d->FillGeometry(cut.get(), brush);
            if (auto rest = combine(bolt.get(), levelShape.get())) {
                brush->SetColor(ink);
                d2d->FillGeometry(rest.get(), brush);
            }
        } else if (levelShape) {
            d2d->FillGeometry(levelShape.get(), brush);
        }
    }
}

// The start entry: the Windows logo, redrawn as four panes with Windows 11's
// light-to-deep blue, slightly smaller than an app icon's square.
void BarPainter::DrawLogo(ID2D1DeviceContext2* d2d, D2D1_RECT_F r, float opacity)
{
    float w = std::min(r.right - r.left, r.bottom - r.top) * 0.80f;
    float cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
    float left = cx - w / 2, top = cy - w / 2;
    float gap = w * 0.08f, pane = (w - gap) / 2, corner = pane * 0.10f;
    if (!logoBlue_) {
        D2D1_GRADIENT_STOP stops[] = {{0.f, D2D1::ColorF(0x5CCBFF)}, {0.5f, D2D1::ColorF(0x1A9BF0)}, {1.f, D2D1::ColorF(0x0060C8)}};
        winrt::com_ptr<ID2D1GradientStopCollection> coll;
        if (FAILED(d2d->CreateGradientStopCollection(stops, 3, coll.put())) ||
            FAILED(d2d->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties({}, {}), coll.get(), logoBlue_.put())) ||
            FAILED(d2d->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1, 0.16f), logoSheen_.put()))) {
            logoBlue_ = nullptr;
            return;
        }
    }
    ID2D1LinearGradientBrush* grad = logoBlue_.get();
    grad->SetStartPoint(D2D1::Point2F(left, top));
    grad->SetEndPoint(D2D1::Point2F(left + w, top + w));
    grad->SetOpacity(opacity);
    ID2D1SolidColorBrush* sheen = logoSheen_.get();
    sheen->SetOpacity(opacity);
    for (int i = 0; i < 4; ++i) {
        float x = left + float(i % 2) * (pane + gap), y = top + float(i / 2) * (pane + gap);
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(D2D1::RectF(x, y, x + pane, y + pane), corner, corner);
        FillContinuousRect(d2d, rr.rect, rr.radiusX, grad);
        // A faint lit edge on top of each pane.
        d2d->FillRectangle(D2D1::RectF(x + corner, y, x + pane - corner, y + std::max(0.6f, pane * 0.06f)), sheen);
    }
}

} // namespace atrium
