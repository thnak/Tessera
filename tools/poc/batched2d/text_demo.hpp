// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_renderer.hpp"

#include <cstdint>
#include <span>
#include <string>
#include <utility>

namespace tessera::poc::batched2d {

// Small optional text overlay for the batched2D proof. It exercises the real text stack without
// changing the default demo output.
class TextDemo {
public:
    TextDemo(rendering::rhi::Device& device, ui::text::FontFace face, std::string text,
             float pixel_size)
        : face_(std::move(face)),
          rasterizer_(face_),
          shaper_(face_),
          renderer_(device, rasterizer_, {512, 512, 2, 1}),
          text_(std::move(text)),
          pixel_size_(static_cast<std::uint16_t>(pixel_size)) {}

    void begin_frame() {
        renderer_.begin_frame();
    }

    void draw(rendering::gpu::Batched2DRenderer& renderer, float x, float baseline_y,
              rendering::rhi::Color color) {
        ui::text::TextStyle style;
        style.font_size = static_cast<float>(pixel_size_);
        const ui::text::ShapedRun run = shaper_.shape(text_, style);
        renderer_.draw(renderer, run, x, baseline_y, pixel_size_, color);
    }

    void draw_lines(rendering::gpu::Batched2DRenderer& renderer, std::span<const std::string> lines,
                    float x, float first_baseline, float line_height, std::uint16_t pixel_size,
                    rendering::rhi::Color color) {
        ui::text::TextStyle style;
        style.font_size = static_cast<float>(pixel_size);
        float baseline = first_baseline;
        for (const std::string& line : lines) {
            const ui::text::ShapedRun run = shaper_.shape(line, style);
            renderer_.draw(renderer, run, x, baseline, pixel_size, color);
            baseline += line_height;
        }
    }

    // Draws a wrapped paragraph: line boxes come from the shared text layout, each line is shaped
    // and drawn through the same TextRenderer path. Returns the number of lines drawn.
    std::size_t draw_wrapped(rendering::gpu::Batched2DRenderer& renderer, std::string_view text,
                             float x, float first_baseline, float max_width,
                             std::uint16_t pixel_size, rendering::rhi::Color color) {
        ui::text::TextStyle style;
        style.font_size = static_cast<float>(pixel_size);
        ui::text::TextConstraints constraints;
        constraints.wrap = true;
        constraints.max_width = max_width;
        const ui::text::TextLayout layout = ui::text::layout_text(
            text, style, ui::text::StubFontMetricsProvider{}.metrics(style), constraints);
        float baseline = first_baseline;
        std::size_t drawn = 0;
        for (const ui::text::TextLine& line : layout.lines) {
            if (line.end > line.begin) {
                const std::string_view slice = text.substr(line.begin, line.end - line.begin);
                const ui::text::ShapedRun run = shaper_.shape(slice, style);
                renderer_.draw(renderer, run, x, baseline, pixel_size, color);
                ++drawn;
            }
            baseline += line.height;
        }
        return drawn;
    }

    [[nodiscard]] const ui::text::GlyphAtlas& atlas() const {
        return renderer_.atlas();
    }

private:
    ui::text::FontFace face_;
    ui::text::GlyphRasterizer rasterizer_;
    ui::text::HarfBuzzShaper shaper_;
    ui::text::TextRenderer renderer_;
    std::string text_;
    std::uint16_t pixel_size_ = 16;
};

} // namespace tessera::poc::batched2d
