// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

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
};

} // namespace tessera::ui::paint
