// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstdint>

namespace tessera::ui::scale {

// The render-target consumer of the adaptive-quality decision (docs/design/performance.md section
// 6, docs/specs/runtime-config-v1.md section 6.2). Two scales are kept apart here:
//
// - the device scale: the monitor-derived value, quantized onto kRenderScaleLadder, which sizes the
//   window's physical pixel grid and keys glyph rasterization / the text atlas (window-resize.md
//   section 2.3). It never changes when quality changes.
// - the adaptive scale: the controller's renderScale decision (src/runtime/adaptive_quality.hpp),
//   applied on top of the presented pixel grid. Only the scene's render target changes size; the
//   window, the swapchain, the layout and the glyph atlas do not.

// The full-quality pixel size of a window: logical size times the device scale, rounded up to whole
// pixels with a minimum of 1 and clamped to the RHI's extent ceiling. This is the window's
// presented extent (its physical pixel grid), and the base the adaptive scale multiplies.
inline constexpr std::uint32_t kMaxRenderTargetExtent = 65535;

[[nodiscard]] std::uint32_t presented_extent_pixels(float logical, float device_scale) noexcept;

// The device (ladder) scale that glyph rasterization and the text atlas are keyed by: the reported
// dpi scale quantized onto kRenderScaleLadder. Adaptive quality is never an input here, which is
// what keeps a downgrade from re-rasterizing text.
[[nodiscard]] float glyph_raster_scale(float reported_device_scale) noexcept;

// One axis of the adaptive target: floor(presented * adaptive_scale), minimum 1, clamped to
// kMaxRenderTargetExtent. A non-finite or non-positive adaptive scale is treated as 1.0, so a bad
// decision can never collapse the target to nothing.
[[nodiscard]] std::uint32_t adaptive_extent_pixels(std::uint32_t presented,
                                                   float adaptive_scale) noexcept;

// What the window renders into for one frame. At `extent == presented` the frame takes the existing
// full-quality direct path (no offscreen target, no resample), which is the bit-identical guarantee
// at adaptive scale 1.0.
struct AdaptiveTargetExtent {
    // Extent the scene is rendered into; never zero.
    rendering::rhi::Extent2D extent;
    // Extent the swapchain presents at (the window's physical size).
    rendering::rhi::Extent2D presented;
    // True when the scene must render into a separate target and be resampled up on present.
    bool scaled = false;
    // Scene coordinates (presented pixels) -> target pixels, per axis. Exactly 1.0 when !scaled;
    // the two axes can differ by a fraction of a pixel because `extent` is rounded down.
    float content_scale_x = 1.0f;
    float content_scale_y = 1.0f;
};

// The whole decision: presented extent in, adaptive target out.
//
// The base is the *presented* extent, not the unrounded logical x device product: the platform's
// physical grid is a whole number of pixels (ceil), and using it is the only formulation that is
// exactly identity at adaptive scale 1.0 on every DPI - including fractional ones such as 1.0833,
// where the ladder value and the reported value differ. `presented` is therefore
// presented_extent_pixels(logical, device_scale) rounded up by the platform.
[[nodiscard]] AdaptiveTargetExtent adaptive_target_extent(rendering::rhi::Extent2D presented,
                                                          float adaptive_scale) noexcept;

// The composed CSS -> render-target factor a frame applies with
// `Batched2DRenderer::set_content_scale` (display-aware-rendering.md section 6). It is the window's
// device scale times the adaptive render-scale factor from AdaptiveTargetExtent (target/presented,
// per axis). Scene geometry is authored in CSS px, so this one value is what makes a glyph quad
// placed back in CSS units cover exactly its device-pixel bitmap at a device scale of 2, and what
// lets the rounded-clip and border-snapping math (both in CSS px) land on the target's device grid.
struct ContentScale {
    float x = 1.0f;
    float y = 1.0f;

    // True only when both axes are exactly 1.0, i.e. the pre-transform call path: a frame that sets
    // this would be byte-identical anyway, so a caller can skip set_content_scale() entirely.
    [[nodiscard]] bool is_identity() const noexcept {
        return x == 1.0f && y == 1.0f;
    }

    friend bool operator==(const ContentScale&, const ContentScale&) = default;
};

// Composes the two independent factors. A non-finite or non-positive input falls back to 1 for that
// axis, so a bad metric or a bad quality decision can never collapse or mirror the frame; the
// identity inputs return exactly (1, 1).
[[nodiscard]] ContentScale content_scale_for(float device_scale, float adaptive_x = 1.0f,
                                             float adaptive_y = 1.0f) noexcept;

} // namespace tessera::ui::scale
