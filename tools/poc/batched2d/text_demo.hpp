// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/ui/text/font_face.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/harfbuzz_shaper.hpp"
#include "tessera/ui/text/text_renderer.hpp"

#include <cstdint>
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
