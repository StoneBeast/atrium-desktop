// Stable popup viewport: arrivals move cards instead of resizing their glass window.
#pragma once

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstddef>

namespace atrium::notification {

struct PopupLayout {
    RECT rect;
    float viewHeight, contentHeight;
    float contentTop; // notification centre reserves its control capsule above the cards
};

inline PopupLayout TopRightLayout(const RECT& work, float scale, float width, float card, float gap, float edge, size_t count, float controls = 0)
{
    const int availableW = std::max(1L, work.right - work.left), availableH = std::max(1L, work.bottom - work.top);
    const int margin = std::clamp(int(std::lround(edge * scale)), 0, (std::min(availableW, availableH) - 1) / 2);
    const int w = std::clamp(int(std::lround(width * scale)), 1, availableW - 2 * margin);
    const int h = availableH - 2 * margin;
    const LONG right = work.right - margin, top = work.top + margin;
    const float contentTop = std::min(controls, float(h) / scale);
    return {{right - w, top, right, top + h}, float(h) / scale - contentTop,
        count ? float(count) * card + float(count - 1) * gap : 0.f, contentTop};
}

inline float CardTop(size_t index, float card, float gap, float scroll)
{
    return float(index) * (card + gap) - scroll;
}

} // namespace atrium::notification
