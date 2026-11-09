// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

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

// Receives the flat, axis-aligned paint commands produced by paint_scene(). The pass owns no
// draw list and no backend state: a recording sink is enough to test it, and Batched2DSink
// forwards the same commands to rendering::gpu::Batched2DRenderer.
class PaintSink {
public:
    virtual ~PaintSink() = default;

    virtual void fill_rect(const rhi::Rect& rect, const rhi::Color& color) = 0;
    virtual void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color,
                                   float radius) = 0;

    // Optional rounded-clip hooks (default no-ops so sinks that do not render clipping keep
    // working). paint_scene pushes one clip per overflow-clipping parent around its children and
    // pops it afterwards; push/pop are balanced in paint order.
    virtual void push_clip(const RoundedClip& /*clip*/) {}
    virtual void pop_clip() {}

    // Optional text-run hook (default no-op so recording/stub sinks keep compiling). paint_scene
    // emits a node's text after its background/borders and inside the active clip.
    virtual void draw_text(const TextDraw& /*run*/) {}

    // Optional embedded-vector hook (default no-op). Emitted like text: after the node's
    // background/borders, inside the node's own overflow/scroll clip, before its children, and only
    // for PaintKind::Vector payloads. Sinks without a vector backend silently skip the draw.
    virtual void draw_vector(const VectorDraw& /*draw*/) {}
};

} // namespace tessera::ui::paint
