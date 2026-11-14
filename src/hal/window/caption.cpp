// SPDX-License-Identifier: MIT
//
// Caption hit-region rules for the custom title bar (docs/specs/window-chrome-v1.md section 8.1).
//
// Pure and platform-neutral: the same resolver answers for Win32, X11 and the fake window, so the
// hit-test contract is one implementation and one test rather than one per backend. The platform
// files only map a `CaptionRegionKind` to their own non-client answer.

#include "tessera/hal/caption.hpp"

#include <cmath>

namespace tessera::hal {
namespace {

bool usable(const WindowRect& rect) noexcept {
    return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
           std::isfinite(rect.height) && rect.width > 0.0f && rect.height > 0.0f;
}

bool inside(const WindowRect& rect, float x, float y) noexcept {
    // Half-open on the right/bottom edge, like every layout box in the engine: the shared boundary
    // belongs to the next region, so controls laid side by side never both claim a point.
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
}

} // namespace

std::array<CaptionRegion, kMaxCaptionRegions> make_caption_regions(
    std::span<const CaptionRegion> regions, std::uint8_t& count) noexcept {
    std::array<CaptionRegion, kMaxCaptionRegions> kept{};
    count = 0;
    for (const CaptionRegion& region : regions) {
        if (count == kMaxCaptionRegions) {
            break;
        }
        if (usable(region.bounds)) {
            kept[count++] = region;
        }
    }
    return kept;
}

const CaptionRegion* caption_region_at(std::span<const CaptionRegion> regions, float x,
                                       float y) noexcept {
    if (!std::isfinite(x) || !std::isfinite(y)) {
        return nullptr;
    }
    for (const CaptionRegion& region : regions) {
        if (inside(region.bounds, x, y)) {
            return &region;
        }
    }
    return nullptr;
}

const char* caption_region_kind_name(CaptionRegionKind kind) noexcept {
    switch (kind) {
    case CaptionRegionKind::Drag:
        return "drag";
    case CaptionRegionKind::SystemMenu:
        return "system-menu";
    case CaptionRegionKind::Minimize:
        return "minimize";
    case CaptionRegionKind::Maximize:
        return "maximize";
    case CaptionRegionKind::Close:
        return "close";
    }
    return "unknown";
}

} // namespace tessera::hal
