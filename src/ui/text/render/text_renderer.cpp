// SPDX-License-Identifier: MIT

#include "tessera/ui/scale/render_scale.hpp"
#include "tessera/ui/text/scale_glyph_atlas.hpp"
#include "tessera/ui/text/text_renderer.hpp"

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
        scale_atlas.set_hinting(hinting);
        for (std::vector<PageTexture>& step : textures) {
            step.clear();
        }
        active = &scale_atlas.atlas_for(render_scale);
    }

    rendering::rhi::Device* device = nullptr;
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
    impl_->select(render_scale);
}

float TextRenderer::render_scale() const {
    return impl_->render_scale;
}

void TextRenderer::set_aa_policy(const scale::AaPolicy& policy) {
    // Hinting first: select() releases the step being left, and apply_hinting() re-creates the
    // active atlas, so doing it in this order never leaves `active` pointing at a released slot.
    impl_->apply_hinting(policy.hinting == scale::TextHinting::Light  ? GlyphHinting::Light
                         : policy.hinting == scale::TextHinting::None ? GlyphHinting::None
                                                                      : GlyphHinting::Font);
    impl_->select(policy.render_scale);
    impl_->subpixel_position = policy.subpixel_position;
}

GlyphHinting TextRenderer::hinting() const {
    return impl_->scale_atlas.hinting();
}

bool TextRenderer::subpixel_position() const {
    return impl_->subpixel_position;
}

float TextRenderer::draw(rendering::gpu::Batched2DRenderer& renderer, const ShapedRun& run, float x,
                         float baseline_y, std::uint16_t pixel_size, rendering::rhi::Color color) {
    if (run.glyphs.empty() || pixel_size == 0) {
        return 0.0f;
    }

    GlyphAtlas& atlas = *impl_->active;
    std::vector<Impl::PageTexture>& page_textures = impl_->textures[impl_->active_step];

    // Insert every glyph before touching GPU resources so each page is uploaded at most once for
    // this run.
    impl_->entries.clear();
    impl_->used_pages.clear();
    impl_->subpixels.clear();
    // Subpixel positioning: each glyph origin keeps a quarter-pixel fraction, which selects the
    // atlas bitmap rasterized at that offset. The pen walk must therefore run before the lookup.
    float lookup_pen = x;
    for (const Glyph& glyph : run.glyphs) {
        std::uint8_t subpixel = 0;
        if (impl_->subpixel_position) {
            const float origin = lookup_pen + glyph.x_offset;
            const float fraction = origin - std::floor(origin);
            subpixel = static_cast<std::uint8_t>(std::clamp(
                std::lround(fraction * kSubpixelSteps), 0L, static_cast<long>(kSubpixelSteps) - 1));
        }
        impl_->subpixels.push_back(subpixel);
        lookup_pen += glyph.advance;
        impl_->entries.push_back(atlas.get_glyph(glyph.id, pixel_size, glyph.face, subpixel));
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
            desc.pixels = pixels.data();
            desc.row_pitch = static_cast<std::size_t>(atlas.page_width()) * 4;
            desc.filter = rendering::rhi::Filter::Linear;
            slot.texture = impl_->device->create_texture(desc);
            slot.generation = generation;
            slot.created = true;
        } else if (slot.generation != generation) {
            slot.texture->write(pixels.data(), pixels.size());
            slot.generation = generation;
        }
    }

    // Coverage contrast by the text colour's lightness: none for dark text, up to 1.4 for white.
    const float lightness =
        0.2126f * color.r + 0.7152f * color.g + 0.0722f * color.b; // encoded, not linear: a proxy
    const float boost = 1.0f + 0.4f * std::clamp((lightness - 0.5f) * 2.0f, 0.0f, 1.0f);

    float pen_x = x;
    for (std::size_t i = 0; i < run.glyphs.size(); ++i) {
        const Glyph& glyph = run.glyphs[i];
        const GlyphAtlasEntry& entry = impl_->entries[i];
        if (entry.has_bitmap && entry.page < page_textures.size()) {
            // Glyph bitmaps are sampled with a linear filter, so a quad that does not sit on the
            // pixel grid blends each row with its neighbour: thin strokes go faint and the last
            // descender row fades. Both axes snap to whole pixels (a browser does the same for
            // grayscale text); the pen keeps its fractional advance so spacing does not drift.
            // With subpixel positioning the bitmap already carries the fraction, so the quad sits
            // on the floor of the origin; otherwise the whole origin rounds as before. A fraction
            // that rounded up to the next quarter step stays below 1.0 (clamped above), so floor
            // and the bitmap offset can never disagree by a whole pixel.
            const float left =
                impl_->subpixel_position
                    ? std::floor(pen_x + glyph.x_offset) + static_cast<float>(entry.bearing_x)
                    : std::round(pen_x + glyph.x_offset + static_cast<float>(entry.bearing_x));
            const float top =
                std::round(baseline_y - glyph.y_offset - static_cast<float>(entry.bearing_y));
            const rendering::rhi::Rect uv{
                static_cast<float>(entry.x) / page_width,
                static_cast<float>(entry.y) / page_height,
                static_cast<float>(entry.width) / page_width,
                static_cast<float>(entry.height) / page_height,
            };
            renderer.push_texture(
                {left, top, static_cast<float>(entry.width), static_cast<float>(entry.height)},
                *page_textures[entry.page].texture, uv,
                entry.color ? rendering::rhi::Color{1.0f, 1.0f, 1.0f, color.a} : color,
                entry.color ? 1.0f : boost);
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
