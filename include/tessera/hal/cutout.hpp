// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"

#include <span>
#include <string>

namespace tessera::hal {

// Builds a `CutoutInfo` from the rectangles a platform reports (Android: `DisplayCutout.
// getBoundingRects()` already converted to logical pixels, window coordinates). At most
// `kMaxCutouts` are kept in order and the rest are dropped; a rectangle with a non-finite value or
// no area is skipped. Never allocates and never reads past the span.
[[nodiscard]] CutoutInfo make_cutout_info(std::span<const WindowRect> rects,
                                          const WindowInsets& waterfall_insets = {}) noexcept;

// Deterministic one-line-per-field text dump of the metrics a layout or component test depends on
// (size, scale, insets, cutout rectangles). The headless harness prints it; it never includes a
// pointer or a wall-clock value, so two runs of the same script compare equal.
[[nodiscard]] std::string format_window_metrics(const WindowMetrics& metrics);

} // namespace tessera::hal
