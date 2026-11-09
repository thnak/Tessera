// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace tessera::poc::batched2d {

// CPU implementation of the paint sink used by `batched2d --backend cpu --cpu-text`.
// It reuses the paint pass output (TextDraw per line from PaintData) and the same FreeType glyph
// coverage bitmaps as the GPU text path, but composites them into the deterministic CPU canvas.
// Rounded rects and rounded clips are approximated by their axis-aligned bounds (the CPU golden
// scene uses plain rects); transforms are already folded into the paint rects.
class CpuPaintSink final : public ui::paint::PaintSink {
public:
    CpuPaintSink(rendering::software::CpuCanvas& canvas, ui::text::GlyphRasterizer& rasterizer,
                 ui::text::TextShaper& shaper)
        : canvas_(&canvas),
          rasterizer_(&rasterizer),
          shaper_(&shaper),
          clip_{0.0f, 0.0f, static_cast<float>(canvas.width()),
                static_cast<float>(canvas.height())} {}

    void fill_rect(const rendering::rhi::Rect& rect, const rendering::rhi::Color& color) override {
        const rendering::rhi::Rect clipped = intersect(clip_, rect);
        if (clipped.width > 0.0f && clipped.height > 0.0f) {
            canvas_->fill_rect(clipped, color);
        }
    }

    void fill_rounded_rect(const rendering::rhi::Rect& rect, const rendering::rhi::Color& color,
                           float /*radius*/) override {
        // The CPU golden scene does not use rounded rects; bounds-only approximation documented.
        fill_rect(rect, color);
    }

    void push_clip(const ui::paint::RoundedClip& clip) override {
        stack_.push_back(clip_);
        clip_ = intersect(clip_, clip.rect);
    }

    void pop_clip() override {
        if (!stack_.empty()) {
            clip_ = stack_.back();
            stack_.pop_back();
        }
    }

    void draw_text(const ui::paint::TextDraw& run) override {
        if (run.text.empty()) {
            return;
        }
        ui::text::TextStyle style;
        style.font_size = run.font_size;
        style.font_weight = run.font_weight;
        const ui::text::ShapedRun shaped = shaper_->shape(run.text, style);
        if (shaped.glyphs.empty()) {
            return;
        }
        const float size = std::clamp(run.font_size, 1.0f, 4096.0f);
        const auto pixel_size = static_cast<std::uint16_t>(std::lround(size));
        const float baseline = run.baseline.value_or(run.rect.y + shaped.ascent);
        float pen_x = run.rect.x;
        for (const ui::text::Glyph& glyph : shaped.glyphs) {
            const ui::text::GlyphBitmap bitmap = rasterizer_->rasterize_glyph(glyph.id, pixel_size);
            if (!bitmap.coverage.empty()) {
                const auto left = static_cast<std::int32_t>(
                    std::lround(pen_x + glyph.x_offset + static_cast<float>(bitmap.bearing_x)));
                const auto top = static_cast<std::int32_t>(
                    std::lround(baseline - glyph.y_offset - static_cast<float>(bitmap.bearing_y)));
                canvas_->blend_coverage(left, top, bitmap.width, bitmap.height, bitmap.coverage,
                                        run.color, &clip_);
            }
            pen_x += glyph.advance;
        }
    }

private:
    static rendering::rhi::Rect intersect(const rendering::rhi::Rect& lhs,
                                          const rendering::rhi::Rect& rhs) {
        const float left = std::max(lhs.x, rhs.x);
        const float top = std::max(lhs.y, rhs.y);
        const float right = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
        const float bottom = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
        if (left >= right || top >= bottom) {
            return {0.0f, 0.0f, 0.0f, 0.0f};
        }
        return {left, top, right - left, bottom - top};
    }

    rendering::software::CpuCanvas* canvas_ = nullptr;
    ui::text::GlyphRasterizer* rasterizer_ = nullptr;
    ui::text::TextShaper* shaper_ = nullptr;
    rendering::rhi::Rect clip_;
    std::vector<rendering::rhi::Rect> stack_;
};

} // namespace tessera::poc::batched2d
