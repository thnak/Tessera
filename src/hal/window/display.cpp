// SPDX-License-Identifier: MIT
//
// The display capability value rules (docs/specs/os-events-v1.md section 2, ADR-023 rule 7).
//
// A platform reports raw numbers; everything that decides what they mean is here, so the fallbacks
// are testable without a display: an unusable scale resolves to 1.0, an empty work area degrades to
// the window, orientation derives from the logical extent, and a name is read without ever running
// past its buffer. This is the same split as `refresh_rate.cpp`: the platform TU only reads the OS
// value and records whether it could.

#include "tessera/hal/display.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace tessera::hal {
namespace {

[[maybe_unused]] void set_name(DisplayInfo& display, std::string_view name) noexcept {
    const std::size_t copied = std::min(name.size(), kMaxDisplayNameBytes - 1);
    if (copied > 0) {
        std::memcpy(display.name.data(), name.data(), copied);
    }
    display.name[copied] = '\0';
}

} // namespace

std::string_view display_info_name(const DisplayInfo& display) noexcept {
    // The buffer is guaranteed terminated by every builder here and by the platform fillers, but a
    // default-constructed one is zeroed anyway; a bounded scan makes the read safe regardless.
    const auto end = std::find(display.name.begin(), display.name.end(), '\0');
    return std::string_view(display.name.data(),
                            static_cast<std::size_t>(end - display.name.begin()));
}

Orientation orientation_from_extent(std::uint32_t logical_width,
                                    std::uint32_t logical_height) noexcept {
    return logical_width >= logical_height ? Orientation::Landscape : Orientation::Portrait;
}

std::string_view orientation_name(Orientation orientation) noexcept {
    switch (orientation) {
    case Orientation::Landscape:
        return "landscape";
    case Orientation::Portrait:
        return "portrait";
    }
    return "unknown";
}

bool orientation_is_known(Orientation orientation) noexcept {
    return orientation == Orientation::Landscape || orientation == Orientation::Portrait;
}

const DisplayInfo* find_display(std::span<const DisplayInfo> displays, DisplayId id) noexcept {
    if (id == kNoDisplayId) {
        return nullptr;
    }
    for (const DisplayInfo& display : displays) {
        if (display.id == id) {
            return &display;
        }
    }
    return nullptr;
}

bool display_scale_usable(float reported_scale) noexcept {
    return std::isfinite(reported_scale) && reported_scale > 0.0f;
}

float resolve_display_scale(float reported_scale) noexcept {
    return display_scale_usable(reported_scale) ? reported_scale : kAssumedDisplayScale;
}

float display_scale_from_dpi(float dpi) noexcept {
    if (!std::isfinite(dpi) || dpi <= 0.0f) {
        return kAssumedDisplayScale;
    }
    return resolve_display_scale(dpi / 96.0f);
}

WindowRect resolve_work_area(const WindowRect& reported, std::uint32_t logical_width,
                             std::uint32_t logical_height) noexcept {
    // All four components must be finite: a rectangle with a NaN origin but a positive size is
    // still unusable for a layout clamp, so the window's own area is the honest fallback.
    const bool usable = std::isfinite(reported.x) && std::isfinite(reported.y) &&
                        std::isfinite(reported.width) && std::isfinite(reported.height) &&
                        reported.width > 0.0f && reported.height > 0.0f;
    if (usable) {
        return reported;
    }
    return WindowRect{0.0f, 0.0f, static_cast<float>(logical_width),
                      static_cast<float>(logical_height)};
}

} // namespace tessera::hal
