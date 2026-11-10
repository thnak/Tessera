// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <array>
#include <cstddef>

// CSS `box-shadow` parsing and emission. Internal to the paint library. Not installed.
namespace tessera::ui::paint::detail {

namespace rhi = tessera::rendering::rhi;

// At most this many shadows are honored per node; extra ones are ignored (bounded, no allocation).
inline constexpr std::size_t kMaxBoxShadows = 4;

struct BoxShadow {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float blur = 0.0f; // CSS blur radius in px (Gaussian sigma = blur / 2)
    float spread = 0.0f;
    rhi::Color color;
    bool inset = false; // parsed but not painted yet
};

struct BoxShadowList {
    std::array<BoxShadow, kMaxBoxShadows> items;
    std::size_t count = 0;
};

// Parses the comma-separated `box-shadow` list: `[inset]? <x> <y> <blur>? <spread>? <color>?` in
// either color position, lengths in px (or a bare 0). A missing color is `currentColor`. A list
// with any invalid entry is invalid as a whole and yields no shadows, like an invalid CSS
// declaration; `none` and a missing property also yield none.
BoxShadowList parse_box_shadows(const style::ComputedStyle& style, const rhi::Color& current_color);

// How far the painted shadows reach beyond the border box on each side (px, local box space),
// accounting for offsets, spread and blur. Zero when there is no outer shadow.
struct ShadowOutsets {
    float left = 0.0f;
    float top = 0.0f;
    float right = 0.0f;
    float bottom = 0.0f;
};
ShadowOutsets shadow_outsets(const style::ComputedStyle& style);

// Paints the outer shadows of `rect` beneath the node's own background: one rounded fill for a
// sharp shadow, and a deterministic stack of concentric fills whose cumulative alpha follows the
// Gaussian edge profile for a blurred one. `radius` is the node's uniform corner radius.
void paint_box_shadows(const style::ComputedStyle& style, const rhi::Rect& rect, float radius,
                       float opacity, const rhi::Color& current_color, PaintSink& sink,
                       PaintStats& stats);

} // namespace tessera::ui::paint::detail
