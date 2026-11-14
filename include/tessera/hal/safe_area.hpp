// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"

#include <cstdint>

namespace tessera::hal {

// The inset arithmetic of `SafeArea` (docs/specs/safe-area-v1.md sections 2 to 4), pure and free of
// any OS or scene type: it reads a `WindowMetrics` snapshot and returns insets, never allocates.

// Window sides. `Start` and `End` are the logical sides of the `edges` shorthand: left/right in
// left-to-right layouts, swapped under right-to-left.
enum SafeEdge : std::uint8_t {
    kEdgeTop = 1,
    kEdgeRight = 2,
    kEdgeBottom = 4,
    kEdgeLeft = 8,
    kEdgeStart = 16,
    kEdgeEnd = 32,
    kEdgesAll = kEdgeTop | kEdgeRight | kEdgeBottom | kEdgeLeft,
};

// Which insets count. `Gestures` is off by default (large on gesture navigation).
enum SafeSource : std::uint8_t {
    kSourceSystemBars = 1,
    kSourceCutout = 2,
    kSourceIme = 4,
    kSourceGestures = 8,
    kSourceHinge = 16,
    kSourcesDefault = kSourceSystemBars | kSourceCutout | kSourceIme | kSourceHinge,
};

struct SafeAreaConfig {
    std::uint8_t edges = kEdgesAll;
    std::uint8_t sources = kSourcesDefault;
    // Floor per applying side, so a device with zero insets still keeps a margin.
    float minimum = 0.0f;
    bool rtl = false;
};

// The inset a cutout rectangle imposes on each window edge it abuts (within one logical pixel):
// a top-center notch gives a top inset only. Zero for a rectangle that touches no edge.
[[nodiscard]] WindowInsets cutout_insets(const WindowMetrics& metrics) noexcept;

// The inset that keeps a single pane out of an occluding hinge: content takes the larger side of
// the hinge, so the inset is the other side plus the hinge. Zero unless `fold.occlusion` is Full.
[[nodiscard]] WindowInsets hinge_insets(const WindowMetrics& metrics) noexcept;

// Per side: the largest inset of the enabled sources (they overlap, an IME inset already includes
// the bars under it), raised to `minimum`, and zero on a side `edges` does not select. Non-finite
// and negative source values count as zero.
[[nodiscard]] WindowInsets resolve_safe_area(const WindowMetrics& metrics,
                                             const SafeAreaConfig& config) noexcept;

} // namespace tessera::hal
