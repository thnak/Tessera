// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/ui/scale/aa_policy.hpp"
#include "tessera/ui/text/glyph_atlas.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::ui::text {

// Turns shaped runs into atlas-textured quads through Batched2DRenderer.
//
// The renderer owns one glyph atlas per render-scale ladder step (`ScaleGlyphAtlas`) and draws
// from the atlas of the active `render_scale` (default 1.0). Page textures are keyed by (ladder
// step, page), so two steps never share a page. A ladder change releases only the step being
// left - its atlas entry and its page textures - and the next draw re-rasterizes that step on
// demand; the other steps are untouched. The render scale is a cache/rasterization key: draw()
// takes the CSS pixel size, rasterizes each glyph at `css_px * render_scale` device pixels, and
// places the quad back in CSS units, so layout, the pen and every advance stay in CSS pixels
// (display-aware-rendering.md sections 3.1). The rasterizer(s) must outlive the renderer.
class TextRenderer {
public:
    TextRenderer(rendering::rhi::Device& device, GlyphRasterizer& rasterizer,
                 GlyphAtlasConfig config = {});
    // A font stack: glyph `face` i is rasterized by `rasterizers[i]` (see FallbackShaper).
    TextRenderer(rendering::rhi::Device& device, std::vector<GlyphRasterizer*> rasterizers,
                 GlyphAtlasConfig config = {});
    ~TextRenderer();

    TextRenderer(const TextRenderer&) = delete;
    TextRenderer& operator=(const TextRenderer&) = delete;

    void begin_frame();

    // Quantizes through scale::quantize_render_scale and switches the active atlas. Switching to
    // the step already active is a no-op, so no glyph is rebuilt; switching to a different step
    // drops the previous step's atlas and page textures. A DPI/monitor-change call, never a
    // frame-path call, and it belongs between frames: releasing a step while quads that reference
    // its pages are still batched would leave those quads with a dead texture.
    void set_render_scale(float render_scale);
    [[nodiscard]] float render_scale() const;

    // Applies the per-window AA policy (display-aware-rendering.md sections 3.2 and 3.3): the
    // policy's render scale, its glyph hinting and whether glyph origins keep a quarter-pixel
    // fraction. A hinting change releases every atlas and page texture (their bitmaps are stale);
    // everything else behaves like set_render_scale(). Same timing rule: between frames only. The
    // default state is the golden policy (font hints, whole-pixel glyph origins), so a renderer
    // that never calls this draws exactly as before.
    void set_aa_policy(const scale::AaPolicy& policy);
    [[nodiscard]] GlyphHinting hinting() const;
    [[nodiscard]] bool subpixel_position() const;

    // Draws the run with its baseline at (x, baseline_y) and returns the pixel advance.
    // `pixel_size` is the run's size in CSS pixels: the bitmap is rasterized at `round(pixel_size *
    // render_scale)` device pixels and the quad is placed back in CSS units, so a caller's
    // coordinates and the returned advance never depend on the display scale.
    float draw(rendering::gpu::Batched2DRenderer& renderer, const ShapedRun& run, float x,
               float baseline_y, std::uint16_t pixel_size, rendering::rhi::Color color);

    // The atlas of the active render-scale step. Always valid: construction creates the floor
    // step's atlas.
    [[nodiscard]] const GlyphAtlas& atlas() const;
    [[nodiscard]] GlyphAtlas& atlas();

    // Live page textures across every ladder step. Diagnostics/tests: a ladder change releases the
    // previous step's textures, so this drops before the next draw rebuilds them on demand.
    [[nodiscard]] std::uint32_t page_texture_count() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tessera::ui::text
