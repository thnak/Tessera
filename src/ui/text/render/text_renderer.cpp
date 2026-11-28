// SPDX-License-Identifier: MIT

#include "tessera/ui/scale/render_scale.hpp"
#include "tessera/ui/text/glyph_prewarm.hpp"
#include "tessera/ui/text/scale_glyph_atlas.hpp"
#include "tessera/ui/text/text_renderer.hpp"
#include "text_contrast_lut.hpp"
#include "text_quad.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace tessera::ui::text {

namespace {

// Ladder position of an already-quantized scale. quantize_render_scale only returns ladder
// values, so the index is always valid; the floor guard only keeps the cast well-defined if the
// ladder ever loses its minimum.
std::size_t ladder_index(float render_scale) noexcept {
    const std::int32_t index = scale::render_scale_index(render_scale);
    return static_cast<std::size_t>(index > 0 ? index : 0);
}

} // namespace

struct TextRenderer::Impl {
    struct PageTexture {
        std::unique_ptr<rendering::rhi::Texture> texture;
        std::uint32_t generation = 0;
        bool created = false;
    };

    Impl(rendering::rhi::Device& device, std::vector<GlyphRasterizer*> rasterizers,
         GlyphAtlasConfig config)
        : device(&device),
          rasterizers(rasterizers),
          scale_atlas(std::move(rasterizers), config) {
        select(scale::kMinRenderScale);
    }

    // The only place the active step changes, so the "release the step we leave" rule cannot
    // drift between construction and set_render_scale().
    void select(float requested_scale) {
        const float quantized = scale::quantize_render_scale(requested_scale);
        if (active != nullptr && quantized == render_scale) {
            return;
        }
        if (active != nullptr) {
            // The DPI-change contract: the previous step is the only one released. Its page
            // textures go with it; the next draw at that step rebuilds them on demand.
            scale_atlas.invalidate(render_scale);
            textures[ladder_index(render_scale)].clear();
        }
        render_scale = quantized;
        active_step = ladder_index(quantized);
        active = &scale_atlas.atlas_for(quantized);
    }

    // Hinting change: every atlas and page texture is stale. The active atlas is rebuilt in place
    // (its pointer died with the release), so the next draw re-rasterizes on demand.
    void apply_hinting(GlyphHinting hinting) {
        if (hinting == scale_atlas.hinting()) {
            return;
        }
        prewarm.cancel();
        scale_atlas.set_hinting(hinting);
        for (std::vector<PageTexture>& step : textures) {
            step.clear();
        }
        active = &scale_atlas.atlas_for(render_scale);
    }

    // AA-mode change: same contract as a hinting change, for the same reason - the pages hold
    // bitmaps rasterized in the old mode, so every one of them is stale.
    void apply_aa_mode(GlyphAaMode aa_mode) {
        if (aa_mode == scale_atlas.aa_mode()) {
            return;
        }
        prewarm.cancel();
        scale_atlas.set_aa_mode(aa_mode);
        for (std::vector<PageTexture>& step : textures) {
            step.clear();
        }
        active = &scale_atlas.atlas_for(render_scale);
    }

    rendering::rhi::Device* device = nullptr;
    // The same stack the scale atlases draw from; the prewarm clones its faces on the worker.
    std::vector<GlyphRasterizer*> rasterizers;
    bool subpixel_position = false;
    std::vector<std::uint8_t> subpixels;
    ScaleGlyphAtlas scale_atlas;
    float render_scale = scale::kMinRenderScale;
    std::size_t active_step = 0;
    GlyphAtlas* active = nullptr;
    // Indexed by ladder step first: a page number is only unique inside one step's atlas.
    std::array<std::vector<PageTexture>, scale::kRenderScaleStepCount> textures;
    std::vector<GlyphAtlasEntry> entries;
    std::vector<std::uint16_t> used_pages;
    GlyphPrewarm prewarm;
    float pending_scale = scale::kMinRenderScale;
};

TextRenderer::TextRenderer(rendering::rhi::Device& device, GlyphRasterizer& rasterizer,
                           GlyphAtlasConfig config)
    : impl_(std::make_unique<Impl>(device, std::vector<GlyphRasterizer*>{&rasterizer}, config)) {}

TextRenderer::TextRenderer(rendering::rhi::Device& device,
                           std::vector<GlyphRasterizer*> rasterizers, GlyphAtlasConfig config)
    : impl_(std::make_unique<Impl>(device, std::move(rasterizers), config)) {}

TextRenderer::~TextRenderer() = default;

void TextRenderer::begin_frame() {
    impl_->scale_atlas.begin_frame();
}

void TextRenderer::set_render_scale(float render_scale) {
    if (scale::quantize_render_scale(render_scale) != impl_->render_scale) {
        impl_->prewarm.cancel();
    }
    impl_->select(render_scale);
}

bool TextRenderer::begin_render_scale_step(float render_scale, runtime::IoTaskExecutor& executor) {
    const float target = scale::quantize_render_scale(render_scale);
    if (target == impl_->render_scale) {
        impl_->prewarm.cancel();
        return true;
    }
    if (impl_->prewarm.pending() && impl_->pending_scale == target) {
        return true;
    }
    // The working set of the step being left, mapped to the device sizes the new step needs. The
    // CSS size is recovered from the device size by the inverse of draw()'s rounding, which is
    // exact for a ladder at or above 1.0; a miss here only means one glyph is rasterized on demand.
    std::vector<GlyphAtlasKey> keys = impl_->active->working_set();
    for (GlyphAtlasKey& key : keys) {
        const long css =
            std::max(1L, std::lround(static_cast<float>(key.pixel_size) / impl_->render_scale));
        key.pixel_size =
            detail::device_pixel_size(static_cast<std::uint16_t>(std::min(css, 65535L)), target);
    }
    keys.erase(std::unique(keys.begin(), keys.end(),
                           [](const GlyphAtlasKey& lhs, const GlyphAtlasKey& rhs) {
                               return lhs.face == rhs.face && lhs.pixel_size == rhs.pixel_size &&
                                      lhs.glyph_index == rhs.glyph_index &&
                                      lhs.x_subpixel == rhs.x_subpixel;
                           }),
               keys.end());
    if (keys.empty()) {
        // Nothing drawn yet, so there is nothing to rebuild: switching costs nothing.
        impl_->prewarm.cancel();
        impl_->select(target);
        return true;
    }
    if (!impl_->prewarm.start(executor, impl_->rasterizers, std::move(keys),
                              impl_->scale_atlas.hinting(), impl_->scale_atlas.aa_mode())) {
        return false;
    }
    impl_->pending_scale = target;
    return true;
}

bool TextRenderer::poll_render_scale_step() {
    if (!impl_->prewarm.ready()) {
        return false;
    }
    // Selecting releases the step being left, so this belongs between frames like every other
    // scale change. The new atlas starts empty and the prewarmed glyphs are placed into it.
    impl_->select(impl_->pending_scale);
    static_cast<void>(impl_->prewarm.install(*impl_->active));
    return true;
}

bool TextRenderer::render_scale_step_pending() const {
    return impl_->prewarm.pending();
}

float TextRenderer::render_scale() const {
    return impl_->render_scale;
}

void TextRenderer::set_aa_policy(const scale::AaPolicy& policy) {
    // Hinting and AA mode first: select() releases the step being left, and apply_*() re-create the
    // active atlas, so doing it in this order never leaves `active` pointing at a released slot.
    impl_->apply_hinting(policy.hinting == scale::TextHinting::Light  ? GlyphHinting::Light
                         : policy.hinting == scale::TextHinting::None ? GlyphHinting::None
                                                                      : GlyphHinting::Font);
    // `LcdWhenEligible` is the window-level permission, not a per-run decision: the atlas is
    // rasterized for LCD and the paint pass's predicate (`TextDraw::lcd`) picks the pipeline per
    // run. An ineligible run draws from the same atlas through the LCD pipeline reading the
    // per-pixel maximum the upload stores in alpha, because the ordinary pipeline would multiply
    // the page's RGB coverage into the ink.
    if (scale::quantize_render_scale(policy.render_scale) != impl_->render_scale) {
        impl_->prewarm.cancel();
    }
    impl_->apply_aa_mode(policy.text_mode == scale::TextAaMode::LcdWhenEligible
                             ? GlyphAaMode::Lcd
                             : GlyphAaMode::Grayscale);
    impl_->select(policy.render_scale);
    impl_->subpixel_position = policy.subpixel_position;
}

GlyphHinting TextRenderer::hinting() const {
    return impl_->scale_atlas.hinting();
}

GlyphAaMode TextRenderer::aa_mode() const {
    return impl_->scale_atlas.aa_mode();
}

bool TextRenderer::subpixel_position() const {
    return impl_->subpixel_position;
}

float TextRenderer::draw(rendering::gpu::Batched2DRenderer& renderer, const ShapedRun& run, float x,
                         float baseline_y, std::uint16_t pixel_size, rendering::rhi::Color color,
                         bool lcd) {
    if (run.glyphs.empty() || pixel_size == 0) {
        return 0.0f;
    }

    // The caller's pixel size is CSS: layout, the pen and every advance stay there. The bitmap is
    // rasterized at the device pixel size it is sampled at (display-aware-rendering.md
    // section 3.1), and the quad converts the bitmap's bearings back into CSS below.
    const float render_scale = impl_->render_scale;
    const std::uint16_t device_size = detail::device_pixel_size(pixel_size, render_scale);

    GlyphAtlas& atlas = *impl_->active;
    std::vector<Impl::PageTexture>& page_textures = impl_->textures[impl_->active_step];

    // Insert every glyph before touching GPU resources so each page is uploaded at most once for
    // this run.
    impl_->entries.clear();
    impl_->used_pages.clear();
    impl_->subpixels.clear();
    // Subpixel positioning: each glyph origin keeps a quarter-device-pixel fraction, which selects
    // the atlas bitmap rasterized at that offset. The fraction is taken in device space, because
    // that is the grid the bitmap is hinted to; the pen walk must therefore run before the lookup.
    float lookup_pen = x;
    for (const Glyph& glyph : run.glyphs) {
        std::uint8_t subpixel = 0;
        if (impl_->subpixel_position) {
            const float origin = (lookup_pen + glyph.x_offset) * render_scale;
            const float fraction = origin - std::floor(origin);
            subpixel = static_cast<std::uint8_t>(std::clamp(
                std::lround(fraction * kSubpixelSteps), 0L, static_cast<long>(kSubpixelSteps) - 1));
        }
        impl_->subpixels.push_back(subpixel);
        lookup_pen += glyph.advance;
        impl_->entries.push_back(atlas.get_glyph(glyph.id, device_size, glyph.face, subpixel));
        const GlyphAtlasEntry& entry = impl_->entries.back();
        if (entry.has_bitmap && std::find(impl_->used_pages.begin(), impl_->used_pages.end(),
                                          entry.page) == impl_->used_pages.end()) {
            impl_->used_pages.push_back(entry.page);
        }
    }

    const float page_width = static_cast<float>(atlas.page_width());
    const float page_height = static_cast<float>(atlas.page_height());
    for (const std::uint16_t page : impl_->used_pages) {
        if (page >= page_textures.size()) {
            page_textures.resize(static_cast<std::size_t>(page) + 1);
        }
        Impl::PageTexture& slot = page_textures[page];
        const std::uint32_t generation = atlas.page_generation(page);
        const std::span<const std::uint8_t> pixels = atlas.page_pixels(page);
        if (!slot.created) {
            rendering::rhi::TextureDesc desc;
            desc.extent = {atlas.page_width(), atlas.page_height()};
            desc.format = rendering::rhi::TextureFormat::R8G8B8A8Unorm;
            // No pixels: creating from the page would upload every texel, 97% of them empty
            // (issue #949). The used extent is written right below.
            desc.filter = rendering::rhi::Filter::Linear;
            // The atlas is a bounded pool with its own budget row, so it is charged to its own
            // category rather than to `Texture` (RHI 1.14, issue #703).
            desc.category = rendering::rhi::MemoryCategory::GlyphAtlas;
            slot.texture = impl_->device->create_texture(desc);
            slot.generation = generation;
            slot.created = true;
            const GlyphAtlasRect used = atlas.take_used_extent(page);
            if (!used.empty()) {
                const std::size_t page_pitch = static_cast<std::size_t>(atlas.page_width()) * 4;
                slot.texture->write_region(
                    rendering::rhi::TextureRegion{used.x, used.y, used.width, used.height},
                    pixels.data() + static_cast<std::size_t>(used.y) * page_pitch +
                        static_cast<std::size_t>(used.x) * 4,
                    page_pitch);
            }
        } else if (slot.generation != generation) {
            // Only the texels written since the last upload: a glyph that adds a few KB to a
            // 2048x2048 page uploads those KB, not 16 MiB (issue #681). An empty rectangle with a
            // changed generation is not expected; the whole page is the safe answer if it happens.
            const GlyphAtlasRect dirty = atlas.take_dirty(page);
            if (dirty.empty()) {
                slot.texture->write(pixels.data(), pixels.size());
            } else {
                const std::size_t page_pitch = static_cast<std::size_t>(atlas.page_width()) * 4;
                slot.texture->write_region(
                    rendering::rhi::TextureRegion{dirty.x, dirty.y, dirty.width, dirty.height},
                    pixels.data() + static_cast<std::size_t>(dirty.y) * page_pitch +
                        static_cast<std::size_t>(dirty.x) * 4,
                    page_pitch);
            }
            slot.generation = generation;
        }
    }

    // Coverage contrast by the text color's encoded lightness: no boost for dark text, up to 1.4
    // for white (display-aware-rendering.md section 3.5, adopted as a D2 amendment). One table
    // lookup per run, never per glyph, and the table is the curve's only definition.
    const float boost =
        detail::kCoverageContrastLut[detail::contrast_lut_index(color.r, color.g, color.b)];

    float pen_x = x;
    for (std::size_t i = 0; i < run.glyphs.size(); ++i) {
        const Glyph& glyph = run.glyphs[i];
        const GlyphAtlasEntry& entry = impl_->entries[i];
        if (entry.has_bitmap && entry.page < page_textures.size()) {
            // An LCD entry's page extent is three samples per pixel, so its pixel footprint is
            // `width / channels`; the UV still covers the whole physical extent, because that is
            // what the page holds. Grayscale entries have channels == 1 and are unchanged.
            const std::uint32_t channels = entry.channels == 0 ? 1u : entry.channels;
            const std::uint32_t footprint_width = entry.width / channels;
            // Glyph bitmaps are sampled with a linear filter, so a quad that does not sit on the
            // pixel grid blends each row with its neighbour: thin strokes go faint and the last
            // descender row fades. Both axes snap to whole device pixels (a browser does the same
            // for grayscale text); the pen keeps its fractional advance so spacing does not drift.
            // With subpixel positioning the bitmap already carries the fraction, so the quad sits
            // on the floor of the origin; otherwise the whole origin rounds. A fraction that
            // rounded up to the next quarter step stays below 1.0 (clamped above), so floor and the
            // bitmap offset can never disagree by a whole pixel. The helper divides the
            // device-space result back into the caller's CSS space, so layout never sees a device
            // unit.
            const detail::GlyphQuad quad = detail::place_glyph_quad(
                pen_x, baseline_y, glyph.x_offset, glyph.y_offset, entry.bearing_x, entry.bearing_y,
                footprint_width, entry.height, render_scale, impl_->subpixel_position);
            const rendering::rhi::Rect uv{
                static_cast<float>(entry.x) / page_width,
                static_cast<float>(entry.y) / page_height,
                static_cast<float>(entry.width) / page_width,
                static_cast<float>(entry.height) / page_height,
            };
            const rendering::rhi::Rect rect{quad.left, quad.top, quad.width, quad.height};
            rendering::rhi::Texture& page = *page_textures[entry.page].texture;
            if (impl_->scale_atlas.aa_mode() == GlyphAaMode::Lcd && !entry.color) {
                // An LCD page holds coverage in RGB, which the ordinary pipeline would multiply
                // into the ink, so every glyph of the page takes the LCD pipeline. Only an eligible
                // run's three-channel glyph composites per channel; the rest read alpha.
                renderer.push_texture_lcd(rect, page, uv, color, boost, lcd && channels == 3);
            } else {
                renderer.push_texture(rect, page, uv,
                                      entry.color ? rendering::rhi::Color{1.0f, 1.0f, 1.0f, color.a}
                                                  : color,
                                      entry.color ? 1.0f : boost);
            }
        }
        pen_x += glyph.advance;
    }
    return run.advance;
}

const GlyphAtlas& TextRenderer::atlas() const {
    return *impl_->active;
}

GlyphAtlas& TextRenderer::atlas() {
    return *impl_->active;
}

std::uint32_t TextRenderer::page_texture_count() const noexcept {
    std::uint32_t count = 0;
    for (const std::vector<Impl::PageTexture>& step : impl_->textures) {
        count += static_cast<std::uint32_t>(step.size());
    }
    return count;
}

} // namespace tessera::ui::text
