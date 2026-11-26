// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"
#include "tessera/ui/style/style_environment.hpp"

#include <optional>

namespace tessera::ui::style {

// The one mapping from a window's metrics to the standard `env()` variables (ADR-074 section 5), as
// `with_fold` is the one mapping to the media context. The app host and every design-time preview
// call it, so a previewed phone or foldable and a device answer `env()` the same way.
//
// - `safe-area-inset-*`: `hal::resolve_safe_area` with the default `SafeAreaConfig` (the `SafeArea`
//   component's default sources) over `metrics`, the target rather than an animated value, so an
//   inset animation does not re-resolve the cascade per frame.
// - `viewport-segment-*`: the viewport (`viewport_width` x `viewport_height`, logical px: the
//   metrics' own extent can be absent) split around the hinge when the fold separates, with the
//   same rule as `with_fold` (vertical hinge: two segments side by side; horizontal: one above the
//   other). Otherwise one segment, the whole viewport.
// - `preferred-text-scale`: `text_scale` when the caller has one. `WindowMetrics` does not carry
//   the OS text scale, so a host without another source leaves it out and the fallback applies.
[[nodiscard]] StyleEnvironment window_environment(const hal::WindowMetrics& metrics,
                                                  float viewport_width, float viewport_height,
                                                  std::optional<float> text_scale = std::nullopt);

} // namespace tessera::ui::style
