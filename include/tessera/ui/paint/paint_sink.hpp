// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>

namespace tessera::ui::paint {

namespace rhi = tessera::rendering::rhi;

// One resolved text run emitted by the paint pass. The paint pass applies the PaintData override >
// computed style > default precedence and the node opacity before handing this to the sink.
struct TextDraw {
    rhi::Rect rect;        // node border box in absolute coordinates (text is top-aligned in it)
    std::string_view text; // UTF-8; borrowed for the duration of the draw_text call
    float font_size = 16.0f;
    int font_weight = 400;
    // Inline-span italic run style (issue #229). Sinks that cannot select an italic face ignore it;
    // the stubbed/single-face V1 renderers paint the upright face and the flag is carried for the
    // real text stack.
    bool italic = false;
    rhi::Color color{0.0f, 0.0f, 0.0f, 1.0f};
    // Absolute baseline for a line box computed by the paint pass. nullopt keeps the single-line
    // behavior (the sink top-aligns the shaped run inside `rect` using its own ascent).
    std::optional<float> baseline;
    // The run may composite per-channel LCD coverage (display-aware-rendering.md section 3.4). Set
    // by the paint pass from the node-level eligibility rule; sinks without an LCD pipeline ignore
    // it, and false (the default) is the grayscale composite every run used before.
    bool lcd = false;
};

// Clip emitted by the paint pass for a parent with `overflow` clipping. `rect` is the parent's
// border box in absolute coordinates; the four radii are already clamped to half the smaller side.
struct RoundedClip {
    rhi::Rect rect;
    float top_left = 0.0f;
    float top_right = 0.0f;
    float bottom_right = 0.0f;
    float bottom_left = 0.0f;

    [[nodiscard]] bool rounded() const {
        return top_left > 0.0f || top_right > 0.0f || bottom_right > 0.0f || bottom_left > 0.0f;
    }
};

// One resolved vector-asset draw emitted by the paint pass. Coordinates are absolute device pixels:
// `rect` is the node's content box (border box inset by padding) and the whole document is mapped
// into it. `asset` is a borrowed embedded-asset name (valid for the duration of draw_vector).
//
// Raster sizing: `raster_width`/`raster_height` are device pixels (0 in both = derive from the rect
// and the sink device scale); one explicit axis derives the other from the document aspect.
// `scale` > 0 is an explicit uniform multiplier on the sink device scale (0 = sink default). The
// explicit raster size wins over `scale`. `tint` multiplies the sampled colors; its alpha already
// includes node opacity and the payload opacity multiplier.
struct VectorDraw {
    rhi::Rect rect;
    std::string_view asset;
    std::uint32_t raster_width = 0;
    std::uint32_t raster_height = 0;
    float scale = 0.0f;
    rhi::Color tint{1.0f, 1.0f, 1.0f, 1.0f};
};

// One resolved raster-image draw emitted by the paint pass (D12). Fields mirror VectorDraw: `rect`
// is the node's padded content box in absolute device pixels, `asset` is a borrowed embedded-asset
// name, raster size/scale select the upload extent (0 = content box x sink device scale), and
// `tint` already includes node and payload opacity.
struct ImageDraw {
    rhi::Rect rect;
    std::string_view asset;
    std::uint32_t raster_width = 0;
    std::uint32_t raster_height = 0;
    float scale = 0.0f;
    rhi::Color tint{1.0f, 1.0f, 1.0f, 1.0f};
    bool decode_original =
        false; // decode at source size (zoom/edit); default is display size x DPR
};

// One resolved linear-gradient fill. Stops are clamped/sorted by the paint pass and each color's
// alpha already includes node opacity. `clip` is the node's border-rounded box; sinks that cannot
// express rounded clipping may use its rect bounds (documented CPU approximation).
//
// `kMaxDrawStops` is larger than the authored cap (8) because a repeating gradient arrives with its
// ramp already expanded to cover the whole gradient line/radius (issue #613), so a sink draws the
// repeat without a second code path. A non-repeating gradient never uses more than 8.
inline constexpr std::size_t kMaxDrawStops = 32;

struct GradientDraw {
    rhi::Rect rect;
    RoundedClip clip;
    float angle_degrees = 180.0f;
    std::array<LinearGradientStop, kMaxDrawStops> stops{};
    std::size_t stop_count = 0;
};

// One resolved radial-gradient fill (issue #613). `center_x`/`center_y` are the gradient's centre
// in absolute paint coordinates and `radius_x`/`radius_y` are its two radii in px, already resolved
// from the size keyword against `rect` by the paint pass, so a sink never needs the box to place
// the ramp. A circle has `radius_x == radius_y`. Stops follow the same clamp/sort/opacity rules as
// `GradientDraw`, and a repeating gradient arrives with its ramp already expanded (offsets may run
// past 1.0, which the shared sampler clamps).
struct RadialGradientDraw {
    rhi::Rect rect;
    RoundedClip clip;
    float center_x = 0.0f;
    float center_y = 0.0f;
    float radius_x = 0.0f;
    float radius_y = 0.0f;
    std::array<LinearGradientStop, kMaxDrawStops> stops{};
    std::size_t stop_count = 0;
};

// One resolved `backdrop-filter` draw (glass, docs/specs/glass-v1.md sections 2 and 3). Emitted
// once per glass node, before the node's own box shadows/background/borders/content and after
// everything painted earlier in the frame: the sink must read what the frame already holds inside
// the dependency region, blur it, and composite it back clipped to `clip`. A sink that cannot read
// the frame target (the CPU reference sink) reports it as unsupported instead of faking it; a sink
// that can (the batched 2D renderer through the render-pass seam) folds `opacity` into the
// composite so a translucent glass node blends the blurred backdrop over the original.
struct BackdropDraw {
    rhi::Rect rect;     // node border box in absolute paint coordinates
    RoundedClip clip;   // border box shaped by `border-radius` (also the composite clip)
    float sigma = 0.0f; // blur standard deviation in CSS px, already clamped to glassMaxBlurRadius
    float opacity = 1.0f; // node opacity, folded into the composite
    float saturate = 1.0f;
    float brightness = 1.0f;
    float contrast = 1.0f;
};

// Encoded-space (sRGB byte) interpolation over sorted stops; at a duplicate offset the later stop
// wins. Sinks share this so the CPU/GPU ramps agree. Both gradient shapes sample through here, so
// the linear and radial ramps cannot drift.
[[nodiscard]] inline rhi::Color sample_gradient_stops(const LinearGradientStop* stops,
                                                      std::size_t stop_count, float t) {
    if (stop_count == 0) {
        return rhi::Color{0.0f, 0.0f, 0.0f, 0.0f};
    }
    if (stop_count == 1) {
        return stops[0].color;
    }
    t = std::clamp(t, 0.0f, 1.0f);
    for (std::size_t index = 0; index < stop_count; ++index) {
        if (t == stops[index].offset) {
            std::size_t last = index;
            while (last + 1 < stop_count && stops[last + 1].offset == t) {
                ++last;
            }
            return stops[last].color;
        }
    }
    if (t <= stops[0].offset) {
        return stops[0].color;
    }
    for (std::size_t index = 1; index < stop_count; ++index) {
        if (t <= stops[index].offset) {
            const LinearGradientStop& a = stops[index - 1];
            const LinearGradientStop& b = stops[index];
            const float span = b.offset - a.offset;
            const float local = span > 0.0f ? (t - a.offset) / span : 1.0f;
            rhi::Color result;
            result.r = a.color.r + (b.color.r - a.color.r) * local;
            result.g = a.color.g + (b.color.g - a.color.g) * local;
            result.b = a.color.b + (b.color.b - a.color.b) * local;
            result.a = a.color.a + (b.color.a - a.color.a) * local;
            return result;
        }
    }
    return stops[stop_count - 1].color;
}

[[nodiscard]] inline rhi::Color sample_linear_gradient(const GradientDraw& draw, float t) {
    return sample_gradient_stops(draw.stops.data(), draw.stop_count, t);
}

// One band of a tessellated radial gradient: an axis-aligned box plus the colour to fill it with.
// The shared tessellator below produces these, and both sinks draw them through their existing
// solid-quad path, so the CPU and GPU results are the same geometry by construction.
struct RadialBand {
    rhi::Rect rect;
    rhi::Color color;
};

// A conic gradient (issue #617): the ramp sweeps clockwise from `from_angle_degrees` around
// `center_x`/`center_y`, which are absolute canvas coordinates like a radial gradient's centre.
struct ConicGradientDraw {
    rhi::Rect rect;
    RoundedClip clip;
    float center_x = 0.0f;
    float center_y = 0.0f;
    float from_angle_degrees = 0.0f;
    std::array<LinearGradientStop, kMaxDrawStops> stops{};
    std::size_t stop_count = 0;
};
// How many bands a radial gradient is tessellated into, per axis. A radial falloff has no dominant
// axis to band along the way a linear one does, so the gradient's bounding box is cut into a
// kRadialBands x kRadialBands grid and each cell is filled with the ramp colour at its centre. This
// is a bounded approximation (documented in docs/design/paint-pass.md section 13): the error is
// bounded by the colour change across one cell, and a golden pins the exact result. 32 keeps a
// 200 px box at ~6 px cells, which is smooth for the ramps V1 paints without a per-pixel shader.
inline constexpr int kRadialBands = 32;

// Tessellates `draw` into at most kRadialBands * kRadialBands bands, in row-major order, and calls
// `emit` for each. Cells whose centre falls outside the ellipse are skipped, so the fill is the
// ellipse itself rather than its bounding box; a cell that straddles the edge is drawn with the
// colour the ramp gives at its centre, which is the documented edge approximation. `emit` is a
// callable taking `const RadialBand&`.
//
// Both sinks call this, so the CPU reference and the GPU path cannot disagree about the geometry:
// the only difference between them is how a solid quad is rasterized.
template<typename Emit>
void tessellate_radial_gradient(const RadialGradientDraw& draw, Emit&& emit) {
    if (draw.stop_count == 0 || draw.radius_x <= 0.0f || draw.radius_y <= 0.0f ||
        draw.rect.width <= 0.0f || draw.rect.height <= 0.0f) {
        return;
    }
    const float cell_width = draw.rect.width / static_cast<float>(kRadialBands);
    const float cell_height = draw.rect.height / static_cast<float>(kRadialBands);
    for (int row = 0; row < kRadialBands; ++row) {
        const float center_y = draw.rect.y + (static_cast<float>(row) + 0.5f) * cell_height;
        const float normalized_y = (center_y - draw.center_y) / draw.radius_y;
        for (int column = 0; column < kRadialBands; ++column) {
            const float center_x = draw.rect.x + (static_cast<float>(column) + 0.5f) * cell_width;
            const float normalized_x = (center_x - draw.center_x) / draw.radius_x;
            const float distance =
                std::sqrt(normalized_x * normalized_x + normalized_y * normalized_y);
            if (distance > 1.0f) {
                continue;
            }
            // The band is grown by half a pixel on the right/bottom so neighbouring cells meet
            // without a seam under a fractional box (the same trick the linear bands use).
            emit(RadialBand{{center_x - cell_width * 0.5f, center_y - cell_height * 0.5f,
                             cell_width + 0.5f, cell_height + 0.5f},
                            sample_gradient_stops(draw.stops.data(), draw.stop_count, distance)});
        }
    }
}

// One conic band (issue #617): a solid cell of the sweep, in canvas coordinates.
struct ConicBand {
    rhi::Rect rect;
    rhi::Color color;
};

// Tessellates `draw` into at most kRadialBands * kRadialBands bands, in row-major order, and calls
// `emit` for each. A conic gradient has no dominant axis either, so it uses the same bounded grid
// the radial one does: each cell is filled with the ramp colour at its centre, which is the
// documented approximation (docs/design/paint-pass.md section 13). Cells whose centre falls outside
// the box are skipped, so the fill is the box itself rather than its bounding rect.
//
// The angle is measured from 12 o'clock clockwise, the CSS convention: `atan2(dx, -dy)` gives 0 at
// the top and grows clockwise, which is why the y term is negated. A cell exactly at the centre has
// no angle, so it takes the ramp's first stop.
template<typename Emit> void tessellate_conic_gradient(const ConicGradientDraw& draw, Emit&& emit) {
    if (draw.stop_count == 0 || draw.rect.width <= 0.0f || draw.rect.height <= 0.0f) {
        return;
    }
    const float cell_width = draw.rect.width / static_cast<float>(kRadialBands);
    const float cell_height = draw.rect.height / static_cast<float>(kRadialBands);
    constexpr float kTau = 6.283185307179586f;
    for (int row = 0; row < kRadialBands; ++row) {
        const float center_y = draw.rect.y + (static_cast<float>(row) + 0.5f) * cell_height;
        for (int column = 0; column < kRadialBands; ++column) {
            const float center_x = draw.rect.x + (static_cast<float>(column) + 0.5f) * cell_width;
            const float dx = center_x - draw.center_x;
            const float dy = center_y - draw.center_y;
            // `atan2(0, 0)` is 0, which is the documented centre case rather than an error.
            const float degrees = std::atan2(dx, -dy) * (360.0f / kTau);
            float turn = (degrees - draw.from_angle_degrees) / 360.0f;
            turn -= std::floor(turn); // into [0, 1)
            emit(ConicBand{{center_x - cell_width * 0.5f, center_y - cell_height * 0.5f,
                            cell_width + 0.5f, cell_height + 0.5f},
                           sample_gradient_stops(draw.stops.data(), draw.stop_count, turn)});
        }
    }
}

// One resolved `box-shadow` layer (issue #461), outer or inset (issue #612). `box` is the shadow's
// own rounded box, with the offset, the spread and - for a blurred shadow - the Gaussian reach
// already applied by the sink. `carve` is the node's border box: CSS paints an outer shadow only
// outside it (CSS Backgrounds 3 section 7.1.1), corner radius included, so a sink that draws the
// whole layer must not paint it inside that box. `sigma` 0 is a hard-edged shadow (a `0` blur),
// which goes through the same shape rather than a plain fill so the carve applies to it too.
//
// An `inset` shadow is the same draw with the roles swapped: `box` is the node's border box (the
// quad the sink clips to) and `carve` is the *inner* box - the border box moved by the offset and
// shrunk by the spread - so the shadow fills the ring between them and fades inward. The inner
// corner radius is the border radius minus the spread, which the sink derives from the two boxes
// because an engine `border-radius` is one length.
struct BoxShadowDraw {
    rhi::Rect box;
    RoundedClip carve;                        // the node's border box shaped by its border-radius
    rhi::Color color{0.0f, 0.0f, 0.0f, 1.0f}; // alpha already folded with node opacity
    float radius = 0.0f; // box corner radius, grown by the spread like CSS does
    float sigma = 0.0f;  // Gaussian standard deviation (blur / 2); 0 = hard edge
    bool inset = false;  // paint inside `box` rather than outside `carve`
};

// Receives the flat, axis-aligned paint commands produced by paint_scene(). The pass owns no
// draw list and no backend state: a recording sink is enough to test it, and Batched2DSink
// forwards the same commands to rendering::gpu::Batched2DRenderer.
class PaintSink {
public:
    virtual ~PaintSink() = default;

    virtual void fill_rect(const rhi::Rect& rect, const rhi::Color& color) = 0;
    virtual void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color,
                                   float radius) = 0;

    // A uniform border on a rounded box: the rounded `rect` minus the same box inset by `width`.
    // The default draws the four square edge strips, which is what a sink without ring support
    // always produced; a sink that can draw the true outline overrides it.
    virtual void fill_rounded_ring(const rhi::Rect& rect, const rhi::Color& color, float /*radius*/,
                                   float width) {
        const float middle = rect.height - 2.0f * width;
        fill_rect({rect.x, rect.y, rect.width, width}, color);
        fill_rect({rect.x + rect.width - width, rect.y + width, width, middle}, color);
        fill_rect({rect.x, rect.y + rect.height - width, rect.width, width}, color);
        fill_rect({rect.x, rect.y + width, width, middle}, color);
    }

    // Optional linear-gradient hook (default no-op); emitted after the background fill and before
    // borders/content, so it paints where the background would be.
    virtual void fill_linear_gradient(const GradientDraw& /*draw*/) {}

    // Optional radial-gradient hook (default no-op), emitted in the same place as the linear one
    // (issue #613). A sink that does not override it paints the node's background without the
    // gradient, which is the documented fallback for a sink with no radial support.
    virtual void fill_radial_gradient(const RadialGradientDraw& /*draw*/) {}

    // A conic gradient (issue #617). Default no-op so a sink that does not paint one keeps working.
    virtual void fill_conic_gradient(const ConicGradientDraw& /*draw*/) {}

    // Optional rounded-clip hooks (default no-ops so sinks that do not render clipping keep
    // working). paint_scene pushes one clip per overflow-clipping parent around its children and
    // pops it afterwards; push/pop are balanced in paint order.
    virtual void push_clip(const RoundedClip& /*clip*/) {}
    virtual void pop_clip() {}

    // Optional affine-transform hooks for CSS `transform`. A sink that returns true from
    // supports_transform() composes push_transform() onto its current matrix for every later
    // command until the matching pop_transform(); paint_scene then paints a transformed node in
    // its own local frame so rotation, skew and scale draw the real shape. Sinks that keep the
    // default (false) never see these calls and get the node's axis-aligned bounding box instead.
    // Clips and damage regions stay in device space; ClippedPaintSink forwards support from its
    // target.
    [[nodiscard]] virtual bool supports_transform() const {
        return false;
    }
    virtual void push_transform(const rhi::Mat3& /*transform*/) {}
    virtual void pop_transform() {}

    // Optional text-run hook (default no-op so recording/stub sinks keep compiling). paint_scene
    // emits a node's text after its background/borders and inside the active clip.
    virtual void draw_text(const TextDraw& /*run*/) {}

    // Optional embedded-vector hook (default no-op). Emitted like text: after the node's
    // background/borders, inside the node's own overflow/scroll clip, before its children, and only
    // for PaintKind::Vector payloads. Sinks without a vector backend silently skip the draw.
    virtual void draw_vector(const VectorDraw& /*draw*/) {}

    // Optional raster-image hook (default no-op), same placement/precedence rules as draw_vector
    // but for PaintKind::Image payloads. Missing/corrupt assets are silent no-ops in real sinks.
    virtual void draw_image(const ImageDraw& /*draw*/) {}

    // Optional one-shape box shadow (default: unsupported). A sink that returns true draws an outer
    // shadow - sharp or blurred - as the shape in BoxShadowDraw minus the node's border box, so a
    // transparent node shows nothing inside its own box and the Gaussian edge is continuous instead
    // of stepped. The paint pass emits one call per shadow for such a sink and the stack of
    // concentric fills (four strips per layer, so the border box is subtracted there too) for the
    // rest, which is what the CPU reference sinks use.
    [[nodiscard]] virtual bool supports_box_shadow() const {
        return false;
    }
    virtual void fill_box_shadow(const BoxShadowDraw& /*draw*/) {}

    // Optional glass hook (default no-op), emitted before the node's own box visuals. A sink that
    // returns false from supports_backdrop() paints the node without the effect, which is the
    // documented CPU-reference behavior (glass-v1 section 9): the stylesheet's opaque fallback
    // stays visible instead of a blurred approximation that no golden could pin.
    [[nodiscard]] virtual bool supports_backdrop() const {
        return false;
    }
    virtual void draw_backdrop(const BackdropDraw& /*draw*/) {}
};

} // namespace tessera::ui::paint
