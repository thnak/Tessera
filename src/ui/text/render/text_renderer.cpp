// SPDX-License-Identifier: MIT

#include "tessera/ui/scale/render_scale.hpp"
#include "tessera/ui/text/scale_glyph_atlas.hpp"
#include "tessera/ui/text/text_renderer.hpp"

#include <algorithm>
#include <array>
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

    rendering::rhi::Device* device = nullptr;
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
    for (const Glyph& glyph : run.glyphs) {
        impl_->entries.push_back(atlas.get_glyph(glyph.id, pixel_size, glyph.face));
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

    float pen_x = x;
    for (std::size_t i = 0; i < run.glyphs.size(); ++i) {
        const Glyph& glyph = run.glyphs[i];
        const GlyphAtlasEntry& entry = impl_->entries[i];
        if (entry.has_bitmap && entry.page < page_textures.size()) {
            const float left = pen_x + glyph.x_offset + static_cast<float>(entry.bearing_x);
            const float top = baseline_y - glyph.y_offset - static_cast<float>(entry.bearing_y);
            const rendering::rhi::Rect uv{
                static_cast<float>(entry.x) / page_width,
                static_cast<float>(entry.y) / page_height,
                static_cast<float>(entry.width) / page_width,
                static_cast<float>(entry.height) / page_height,
            };
            renderer.push_texture(
                {left, top, static_cast<float>(entry.width), static_cast<float>(entry.height)},
                *page_textures[entry.page].texture, uv,
                entry.color ? rendering::rhi::Color{1.0f, 1.0f, 1.0f, color.a} : color);
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
