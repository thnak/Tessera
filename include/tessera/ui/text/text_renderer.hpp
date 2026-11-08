// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <cstdint>
#include <memory>

namespace tessera::ui::text {

// Turns shaped runs into atlas-textured quads through Batched2DRenderer. One texture per atlas
// page is created on first use and updated in place when that page's generation changes. The
// rasterizer must outlive the renderer.
class TextRenderer {
public:
    TextRenderer(rendering::rhi::Device& device, GlyphRasterizer& rasterizer,
                 GlyphAtlasConfig config = {});
    ~TextRenderer();

    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    void begin_frame();

    // Draws the run with its baseline at (x, baseline_y) and returns the pixel advance.
    float draw(rendering::gpu::Batched2DRenderer& renderer, const ShapedRun& run, float x,
               float baseline_y, std::uint16_t pixel_size, rendering::rhi::Color color);

    [[nodiscard]] const GlyphAtlas& atlas() const;
    [[nodiscard]] GlyphAtlas& atlas();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::ui::text
