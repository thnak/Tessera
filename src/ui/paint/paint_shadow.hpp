// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

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
    bool inset = false; // painted inside the border box (issue #612)
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
    bool blurred = false; // some outer shadow has a blur radius
};
ShadowOutsets shadow_outsets(const style::ComputedStyle& style);

// Which shadows of the list to paint. An outer shadow is painted beneath the node's own background;
// an inset shadow is painted above it (CSS Backgrounds 3 section 7.1.1: the background is painted
// first, then the inset shadow, then the content), so the paint pass calls this twice.
enum class ShadowLayer : std::uint8_t {
    Outer,
    Inset,
};

// Paints the shadows of `rect` of one layer. An outer shadow is never painted inside `rect`'s
// rounded border box: CSS clips an outer shadow inside the border box (CSS Backgrounds 3 section
// 7.1.1), so a transparent node shows nothing where its own box covers the shadow (issue #461). An
// inset shadow (issue #612) is the mirror image: it is painted only inside the rounded border box,
// filling the ring between the border box and the inner box (the border box moved by the offset and
// shrunk by the spread) and fading inward across the blur.
//
// A sink that supports the one-shape box shadow gets one `fill_box_shadow` call per shadow, sharp
// or blurred (the batched 2D shader subtracts the rounded border box per pixel and evaluates the
// Gaussian edge); the others get the layer's area minus `rect` as four strips for an outer shadow
// and the ring between `rect` and the inner box as four strips for an inset one, a deterministic
// stack of concentric fills whose cumulative alpha follows the Gaussian edge profile. `radius` is
// the node's uniform corner radius.
//
// `reduce_effects` caps a blurred shadow at kReducedBlurLayers layers: same box, reach and colour,
// a coarser edge, fewer fills. Sharp shadows are a single layer already and do not change.
void paint_box_shadows(const style::ComputedStyle& style, const rhi::Rect& rect, float radius,
                       float opacity, const rhi::Color& current_color, PaintSink& sink,
                       PaintStats& stats, ShadowLayer layer, bool reduce_effects = false);

inline constexpr int kReducedBlurLayers = 2;

} // namespace tessera::ui::paint::detail
