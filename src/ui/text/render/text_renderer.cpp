// SPDX-License-Identifier: MIT

#include "tessera/ui/text/text_renderer.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace tessera::ui::text {

struct TextRenderer::Impl {
    struct PageTexture {
        std::unique_ptr<rendering::rhi::Texture> texture;
        std::uint32_t generation = 0;
        bool created = false;
    };

    Impl(rendering::rhi::Device& device, GlyphRasterizer& rasterizer, GlyphAtlasConfig config)
        : device(&device),
          atlas(rasterizer, config) {}

    rendering::rhi::Device* device = nullptr;
    GlyphAtlas atlas;
    std::vector<PageTexture> textures;
    std::vector<GlyphAtlasEntry> entries;
    std::vector<std::uint16_t> used_pages;
};

TextRenderer::TextRenderer(rendering::rhi::Device& device, GlyphRasterizer& rasterizer,
                           GlyphAtlasConfig config)
    : impl_(std::make_unique<Impl>(device, rasterizer, config)) {}

TextRenderer::~TextRenderer() = default;

void TextRenderer::begin_frame() {
    impl_->atlas.begin_frame();
}

float TextRenderer::draw(rendering::gpu::Batched2DRenderer& renderer, const ShapedRun& run, float x,
                         float baseline_y, std::uint16_t pixel_size, rendering::rhi::Color color) {
    if (run.glyphs.empty() || pixel_size == 0) {
        return 0.0f;
    }

    // Insert every glyph before touching GPU resources so each page is uploaded at most once for
    // this run.
    impl_->entries.clear();
    impl_->used_pages.clear();
    for (const Glyph& glyph : run.glyphs) {
        impl_->entries.push_back(impl_->atlas.get_glyph(glyph.id, pixel_size));
        const GlyphAtlasEntry& entry = impl_->entries.back();
        if (entry.has_bitmap && std::find(impl_->used_pages.begin(), impl_->used_pages.end(),
                                          entry.page) == impl_->used_pages.end()) {
            impl_->used_pages.push_back(entry.page);
        }
    }

    const float page_width = static_cast<float>(impl_->atlas.page_width());
    const float page_height = static_cast<float>(impl_->atlas.page_height());
    for (const std::uint16_t page : impl_->used_pages) {
        if (page >= impl_->textures.size()) {
            impl_->textures.resize(static_cast<std::size_t>(page) + 1);
        }
        Impl::PageTexture& slot = impl_->textures[page];
        const std::uint32_t generation = impl_->atlas.page_generation(page);
        const std::span<const std::uint8_t> pixels = impl_->atlas.page_pixels(page);
        if (!slot.created) {
            rendering::rhi::TextureDesc desc;
            desc.extent = {impl_->atlas.page_width(), impl_->atlas.page_height()};
            desc.format = rendering::rhi::TextureFormat::R8G8B8A8Unorm;
            desc.pixels = pixels.data();
            desc.row_pitch = static_cast<std::size_t>(impl_->atlas.page_width()) * 4;
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
        if (entry.has_bitmap && entry.page < impl_->textures.size()) {
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
                *impl_->textures[entry.page].texture, uv, color);
        }
        pen_x += glyph.advance;
    }
    return run.advance;
}

const GlyphAtlas& TextRenderer::atlas() const {
    return impl_->atlas;
}

GlyphAtlas& TextRenderer::atlas() {
    return impl_->atlas;
}

} // namespace tessera::ui::text
