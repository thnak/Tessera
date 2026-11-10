// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <array>
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
    rhi::Color color{0.0f, 0.0f, 0.0f, 1.0f};
    // Absolute baseline for a line box computed by the paint pass. nullopt keeps the single-line
    // behavior (the sink top-aligns the shaped run inside `rect` using its own ascent).
    std::optional<float> baseline;
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
};

// One resolved linear-gradient fill. Stops are clamped/sorted by the paint pass and each color's
// alpha already includes node opacity. `clip` is the node's border-rounded box; sinks that cannot
// express rounded clipping may use its rect bounds (documented CPU approximation).
struct GradientDraw {
    rhi::Rect rect;
    RoundedClip clip;
    float angle_degrees = 180.0f;
    std::array<LinearGradientStop, 8> stops{};
    std::size_t stop_count = 0;
};

// Encoded-space (sRGB byte) interpolation over sorted stops; at a duplicate offset the later stop
// wins. Sinks share this so the CPU/GPU ramps agree.
[[nodiscard]] inline rhi::Color sample_linear_gradient(const GradientDraw& draw, float t) {
    if (draw.stop_count == 0) {
        return rhi::Color{0.0f, 0.0f, 0.0f, 0.0f};
    }
    if (draw.stop_count == 1) {
        return draw.stops[0].color;
    }
    t = std::clamp(t, 0.0f, 1.0f);
    for (std::size_t index = 0; index < draw.stop_count; ++index) {
        if (t == draw.stops[index].offset) {
            std::size_t last = index;
            while (last + 1 < draw.stop_count && draw.stops[last + 1].offset == t) {
                ++last;
            }
            return draw.stops[last].color;
        }
    }
    if (t <= draw.stops[0].offset) {
        return draw.stops[0].color;
    }
    for (std::size_t index = 1; index < draw.stop_count; ++index) {
        if (t <= draw.stops[index].offset) {
            const LinearGradientStop& a = draw.stops[index - 1];
            const LinearGradientStop& b = draw.stops[index];
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
    return draw.stops[draw.stop_count - 1].color;
}

// Receives the flat, axis-aligned paint commands produced by paint_scene(). The pass owns no
// draw list and no backend state: a recording sink is enough to test it, and Batched2DSink
// forwards the same commands to rendering::gpu::Batched2DRenderer.
class PaintSink {
public:
    virtual ~PaintSink() = default;

    virtual void fill_rect(const rhi::Rect& rect, const rhi::Color& color) = 0;
    virtual void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color,
                                   float radius) = 0;

    // Optional linear-gradient hook (default no-op); emitted after the background fill and before
    // borders/content, so it paints where the background would be.
    virtual void fill_linear_gradient(const GradientDraw& /*draw*/) {}

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
};

} // namespace tessera::ui::paint
