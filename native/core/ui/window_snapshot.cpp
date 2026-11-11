#include "ui/window_snapshot.h"

#include <dwmapi.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <thread>
#include <utility>

namespace atrium {

namespace {

// Coverage of a rounded rectangle (0..w, 0..h, corners r) at the centre of pixel (x, y).
float RoundedCoverage(int x, int y, int w, int h, float r)
{
    float px = float(x) + 0.5f, py = float(y) + 0.5f;
    float cx = std::clamp(px, r, float(w) - r), cy = std::clamp(py, r, float(h) - r);
    float d = std::hypot(px - cx, py - cy);
    return std::clamp(r + 0.5f - d, 0.f, 1.f);
}

// Area average of `src` (sw x sh BGRA, stride sw) into tw x th.
std::vector<uint8_t> Shrink(const uint8_t* src, int sw, int sh, int stride, int tw, int th)
{
    std::vector<uint32_t> sum(size_t(tw) * th * 3, 0), count(size_t(tw) * th, 0);
    std::vector<int> col(static_cast<size_t>(sw));
    for (int x = 0; x < sw; ++x)
        col[size_t(x)] = std::min(tw - 1, int(int64_t(x) * tw / sw));
    for (int y = 0; y < sh; ++y) {
        const int ty = std::min(th - 1, int(int64_t(y) * th / sh));
        const uint8_t* row = src + size_t(y) * stride;
        uint32_t* s = sum.data() + size_t(ty) * tw * 3;
        uint32_t* n = count.data() + size_t(ty) * tw;
        for (int x = 0; x < sw; ++x) {
            const int tx = col[size_t(x)];
            s[tx * 3] += row[x * 4];
            s[tx * 3 + 1] += row[x * 4 + 1];
            s[tx * 3 + 2] += row[x * 4 + 2];
            ++n[tx];
        }
    }
    std::vector<uint8_t> out(size_t(tw) * th * 4);
    for (size_t i = 0; i < size_t(tw) * th; ++i) {
        const uint32_t n = std::max<uint32_t>(1, count[i]);
        out[i * 4] = uint8_t(sum[i * 3] / n);
        out[i * 4 + 1] = uint8_t(sum[i * 3 + 1] / n);
        out[i * 4 + 2] = uint8_t(sum[i * 3 + 2] / n);
        out[i * 4 + 3] = 255;
    }
    return out;
}

// All but a few of `src`'s pixels (w x h BGRA) are of the colour in its middle.
bool Blank(const uint8_t* src, int w, int h, int stride)
{
    const uint8_t* mid = src + size_t(h / 2) * stride + size_t(w / 2) * 4;
    int all = 0, same = 0;
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) {
            const uint8_t* p = src + size_t(y) * stride + size_t(x) * 4;
            ++all;
            if (std::abs(p[0] - mid[0]) <= 6 && std::abs(p[1] - mid[1]) <= 6 && std::abs(p[2] - mid[2]) <= 6)
                ++same;
        }
    return same >= all - all / 200;
}

} // namespace

// A box's Gaussian blur, axis by axis (the corners' rounding is lost in the blur).
IconImage SoftShadow(int w, int h)
{
    const int pad = kShadowPad / 2, sw = std::max(1, w / 2), sh = std::max(1, h / 2);
    IconImage s;
    s.width = sw + 2 * pad;
    s.height = sh + 2 * pad;
    s.bgra.assign(size_t(s.width) * s.height * 4, 0);
    const float sigma = float(pad) / 2.6f;
    auto edge = [&](float v, float len) {
        auto erfAt = [&](float t) { return 0.5f * (1 + std::erf(t / (sigma * 1.41421356f))); };
        return erfAt(v) - erfAt(v - len);
    };
    std::vector<float> fx(static_cast<size_t>(s.width)), fy(static_cast<size_t>(s.height));
    for (int x = 0; x < s.width; ++x)
        fx[size_t(x)] = edge(float(x) + 0.5f - float(pad), float(sw));
    for (int y = 0; y < s.height; ++y)
        fy[size_t(y)] = edge(float(y) + 0.5f - float(pad), float(sh));
    for (int y = 0; y < s.height; ++y)
        for (int x = 0; x < s.width; ++x)
            s.bgra[(size_t(y) * s.width + x) * 4 + 3] = uint8_t(std::lround(255 * std::clamp(fx[size_t(x)] * fy[size_t(y)], 0.f, 1.f)));
    return s;
}

namespace {

WindowPicture Finish(std::vector<uint8_t> pixels, int w, int h, float radius)
{
    WindowPicture p;
    p.width = w;
    p.height = h;
    p.image.width = w + 2 * kPicturePad;
    p.image.height = h + 2 * kPicturePad;
    p.image.bgra.assign(size_t(p.image.width) * p.image.height * 4, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const float a = RoundedCoverage(x, y, w, h, radius) * (float(pixels[(size_t(y) * w + x) * 4 + 3]) / 255.f);
            const uint8_t* s = &pixels[(size_t(y) * w + x) * 4];
            uint8_t* d = &p.image.bgra[(size_t(y + kPicturePad) * p.image.width + x + kPicturePad) * 4];
            d[0] = uint8_t(std::lround(s[0] * a));
            d[1] = uint8_t(std::lround(s[1] * a));
            d[2] = uint8_t(std::lround(s[2] * a));
            d[3] = uint8_t(std::lround(255 * a));
        }
    p.shadow = SoftShadow(w, h);
    p.ok = true;
    return p;
}

WindowPicture Capture(HWND window, int maxWidth, int maxHeight, float radius)
{
    RECT win{}, frame{};
    if (!GetWindowRect(window, &win) || win.right <= win.left || win.bottom <= win.top)
        return {};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))) || frame.right <= frame.left ||
        frame.bottom <= frame.top)
        frame = win;
    const int ww = win.right - win.left, wh = win.bottom - win.top;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = ww;
    bi.bmiHeader.biHeight = -wh;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP dib = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!dc || !dib || !bits) {
        if (dib)
            DeleteObject(dib);
        if (dc)
            DeleteDC(dc);
        return {};
    }
    HGDIOBJ old = SelectObject(dc, dib);
    // PW_RENDERFULLCONTENT: what DWM shows, also of windows drawn with the GPU.
    const bool printed = PrintWindow(window, dc, 2) != FALSE;
    GdiFlush();
    WindowPicture result;
    if (printed) {
        // The window without its invisible resize borders.
        RECT part{std::max(0L, frame.left - win.left), std::max(0L, frame.top - win.top), std::min<LONG>(ww, frame.right - win.left),
                  std::min<LONG>(wh, frame.bottom - win.top)};
        const int pw = part.right - part.left, ph = part.bottom - part.top;
        const uint8_t* src = static_cast<const uint8_t*>(bits) + (size_t(part.top) * ww + part.left) * 4;
        // Nothing came (some windows cannot be printed), or one plain colour (a window that has not
        // drawn itself yet): no picture.
        if (pw > 0 && ph > 0 && !Blank(src, pw, ph, ww * 4)) {
            const float f = std::min({1.f, float(maxWidth) / float(pw), float(maxHeight) / float(ph)});
            const int tw = std::max(1, int(std::lround(pw * f))), th = std::max(1, int(std::lround(ph * f)));
            result = Finish(Shrink(src, pw, ph, ww * 4, tw, th), tw, th, radius);
        }
    }
    SelectObject(dc, old);
    DeleteObject(dib);
    DeleteDC(dc);
    return result;
}

} // namespace

WindowPicture PlaceholderPicture(const IconImage& icon, int width, int height, bool dark)
{
    std::vector<uint8_t> px(size_t(width) * height * 4);
    const uint8_t tone = dark ? 0x2C : 0xF2;
    for (size_t i = 0; i < size_t(width) * height; ++i) {
        px[i * 4] = px[i * 4 + 1] = px[i * 4 + 2] = tone;
        px[i * 4 + 3] = 255;
    }
    if (!icon.empty()) {
        // The icon (premultiplied) over the panel, in the middle.
        const int ox = (width - icon.width) / 2, oy = (height - icon.height) / 2;
        for (int y = 0; y < icon.height; ++y)
            for (int x = 0; x < icon.width; ++x) {
                const int dx = ox + x, dy = oy + y;
                if (dx < 0 || dy < 0 || dx >= width || dy >= height)
                    continue;
                const uint8_t* s = &icon.bgra[(size_t(y) * icon.width + x) * 4];
                uint8_t* d = &px[(size_t(dy) * width + dx) * 4];
                const int inv = 255 - s[3];
                for (int c = 0; c < 3; ++c)
                    d[c] = uint8_t(std::min(255, s[c] + d[c] * inv / 255));
            }
    }
    return Finish(std::move(px), width, height, float(std::max(6, width / 24)));
}

WindowSnapshots::WindowSnapshots(HWND notify, UINT message) : shared_(std::make_shared<Shared>())
{
    shared_->notify = notify;
    shared_->message = message;
}

WindowSnapshots::~WindowSnapshots()
{
    std::lock_guard g(shared_->lock);
    shared_->notify = nullptr; // workers still out drop what they bring
}

void WindowSnapshots::Request(HWND window, int maxWidth, int maxHeight, float radius)
{
    std::thread([shared = shared_, window, maxWidth, maxHeight, radius] {
        // Physical px of every monitor, as the window's own picture is.
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WindowPicture p = Capture(window, maxWidth, maxHeight, radius);
        std::lock_guard g(shared->lock);
        if (!shared->notify)
            return;
        shared->done.push_back(Result{window, std::move(p)});
        PostMessageW(shared->notify, shared->message, 0, 0);
    }).detach();
}

std::vector<WindowSnapshots::Result> WindowSnapshots::Take()
{
    std::lock_guard g(shared_->lock);
    return std::exchange(shared_->done, {});
}

} // namespace atrium
