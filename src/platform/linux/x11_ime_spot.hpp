// SPDX-License-Identifier: MIT
//
// IME candidate-window anchoring for the X11 backend (issue #55). The application reports the
// caret rectangle in logical pixels (hal::TextInputRect); XIM wants one integer point in window
// pixels (XNSpotLocation): the left edge of the caret at the text baseline side, which is the
// bottom of the rectangle, so the candidate list opens below the line instead of covering it.
// Pure and Xlib-free so the mapping is unit-tested; x11_ime_spot.cpp holds the XSetICValues call.

#pragma once

#include "tessera/hal/window.hpp"

#include <cmath>
#include <cstdint>

namespace tessera::hal::x11 {

struct SpotLocation {
    std::int16_t x = 0;
    std::int16_t y = 0;

    friend bool operator==(const SpotLocation&, const SpotLocation&) = default;
};

// Maps a logical caret rect to a spot in physical window pixels, clamped into the window so a
// caret scrolled outside the viewport cannot park the candidate window off-screen. Returns false
// for a non-finite rectangle or an empty window (nothing sensible to send).
[[nodiscard]] inline bool spot_from_rect(const TextInputRect& rect, float scale,
                                         std::uint32_t window_width, std::uint32_t window_height,
                                         SpotLocation& out) noexcept {
    if (!std::isfinite(rect.x) || !std::isfinite(rect.y) || !std::isfinite(rect.width) ||
        !std::isfinite(rect.height) || !(scale > 0.0F) || window_width == 0 || window_height == 0) {
        return false;
    }
    const float max_x = static_cast<float>(window_width - 1U);
    const float max_y = static_cast<float>(window_height - 1U);
    const float px = std::fmin(std::fmax(rect.x * scale, 0.0F), max_x);
    const float py = std::fmin(std::fmax((rect.y + rect.height) * scale, 0.0F), max_y);
    out.x = static_cast<std::int16_t>(std::lround(std::fmin(px, 32767.0F)));
    out.y = static_cast<std::int16_t>(std::lround(std::fmin(py, 32767.0F)));
    return true;
}

// Remembers the last spot sent so a caret that blinks or a frame that republishes the same rect
// costs no X request.
class SpotTracker {
public:
    // True when `out` holds a spot that differs from the last one accepted.
    [[nodiscard]] bool update(const TextInputRect& rect, float scale, std::uint32_t window_width,
                              std::uint32_t window_height, SpotLocation& out) noexcept {
        SpotLocation next{};
        if (!spot_from_rect(rect, scale, window_width, window_height, next)) {
            return false;
        }
        if (valid_ && next == last_) {
            return false;
        }
        valid_ = true;
        last_ = next;
        out = next;
        return true;
    }
    // A new input context (or a resize that re-anchors) must send the next rect again.
    void reset() noexcept {
        valid_ = false;
    }

private:
    SpotLocation last_{};
    bool valid_ = false;
};

} // namespace tessera::hal::x11
