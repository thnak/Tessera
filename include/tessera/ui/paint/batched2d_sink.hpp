// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_sink.hpp"

namespace tessera::rendering::gpu {
class Batched2DRenderer;
} // namespace tessera::rendering::gpu

namespace tessera::ui::text {
class TextRenderer;
class TextShaper;
} // namespace tessera::ui::text

namespace tessera::ui::paint {

// Forwards paint commands to the batched 2D renderer. Deliberately not a backend adapter: the
// renderer already owns the only GPU seam (rhi::CommandList), so this header stays free of
// Vulkan/WebGPU includes.
//
// Text (D9): draw_text shapes the UTF-8 run with the supplied TextShaper and draws it through the
// existing TextRenderer/glyph-atlas path. Fonts/atlases are owned by the caller; pass both pointers
// to enable text and call `begin_text_frame()` once per painted frame (atlas frame bookkeeping).
class Batched2DSink final : public PaintSink {
public:
    explicit Batched2DSink(rendering::gpu::Batched2DRenderer& renderer,
                           text::TextRenderer* text_renderer = nullptr,
                           text::TextShaper* shaper = nullptr);

    void fill_rect(const rhi::Rect& rect, const rhi::Color& color) override;
    void fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color, float radius) override;
    void push_clip(const RoundedClip& clip) override;
    void pop_clip() override;
    void draw_text(const TextDraw& run) override;

    // Forwards to TextRenderer::begin_frame. Call once per painted frame before any draw_text so
    // atlas pages are not evicted while still referenced by this frame.
    void begin_text_frame();

private:
    rendering::gpu::Batched2DRenderer* renderer_ = nullptr;
    text::TextRenderer* text_renderer_ = nullptr;
    text::TextShaper* shaper_ = nullptr;
};

} // namespace tessera::ui::paint
