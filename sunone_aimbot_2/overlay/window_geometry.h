#pragma once

#include <algorithm>

namespace OverlayWindowGeometry
{
struct Rect
{
    int x, y, width, height;
};

struct Limits
{
    Rect available;
    int minWidth, minHeight;
};

inline Limits limits(Rect work, unsigned dpi)
{
    const auto scaled = [dpi](int value) {
        return static_cast<int>((static_cast<long long>(value) * (dpi ? dpi : 96u) + 48) / 96);
    };
    work.width = std::max(1, work.width);
    work.height = std::max(1, work.height);
    const int marginX = std::min(scaled(10), (work.width - 1) / 2);
    const int marginY = std::min(scaled(10), (work.height - 1) / 2);
    const Rect available{work.x + marginX, work.y + marginY,
        work.width - marginX * 2, work.height - marginY * 2};
    // A preferred minimum must never make the window larger than its monitor.
    return {available, std::min(std::max(1, scaled(560)), available.width),
        std::min(std::max(1, scaled(340)), available.height)};
}

inline Rect fit(Rect desired, Rect work, unsigned dpi, bool center = false)
{
    const auto bounds = limits(work, dpi);
    desired.width = std::clamp(desired.width, bounds.minWidth, bounds.available.width);
    desired.height = std::clamp(desired.height, bounds.minHeight, bounds.available.height);
    const int remainingX = bounds.available.width - desired.width;
    const int remainingY = bounds.available.height - desired.height;
    desired.x = center ? bounds.available.x + remainingX / 2 :
        std::clamp(desired.x, bounds.available.x, bounds.available.x + remainingX);
    desired.y = center ? bounds.available.y + remainingY / 2 :
        std::clamp(desired.y, bounds.available.y, bounds.available.y + remainingY);
    return desired;
}
}
