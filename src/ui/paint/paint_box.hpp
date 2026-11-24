// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <optional>

namespace tessera::ui::paint::detail {

// A border on a rounded box drawn as one ring (uniform width, colour and radius).
struct RingBorder {
    rhi::Color color;
    float width = 0.0f;
    float radius = 0.0f;
};

void paint_background(const style::ComputedStyle& style, const rhi::Rect& rect, float opacity,
                      const rhi::Color& current, PaintSink& sink, PaintStats& stats);

// Four edge strips; a rounded uniform border uses `rounded_ring` instead.
void paint_borders(const style::ComputedStyle& style, const rhi::Rect& rect, float opacity,
                   const rhi::Color& current, float device_scale, PaintSink& sink,
                   PaintStats& stats);

[[nodiscard]] std::optional<RingBorder> rounded_ring(const style::ComputedStyle& style,
                                                     const rhi::Rect& rect, float opacity,
                                                     const rhi::Color& current, float device_scale);

} // namespace tessera::ui::paint::detail
