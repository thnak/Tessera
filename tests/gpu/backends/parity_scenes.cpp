// SPDX-License-Identifier: MIT

#include "parity_scenes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace tessera::tests::gpu::backends {
namespace {

using namespace tessera::rendering;

constexpr std::uint32_t kAtlasCell = 8;
constexpr std::uint32_t kGlyphCount = 6;
constexpr std::uint32_t kAtlasWidth = kAtlasCell * kGlyphCount;
constexpr std::uint32_t kAtlasHeight = kAtlasCell;
constexpr std::uint32_t kTextureSide = 16;

constexpr rhi::Color kInk = rhi::rgba8(232, 238, 247);
constexpr rhi::Color kAccent = rhi::rgba8(96, 165, 250);
constexpr rhi::Color kCard = rhi::rgba8(51, 65, 85);
constexpr rhi::Color kGold = rhi::rgba8(240, 190, 70);

std::unique_ptr<rhi::Texture> make_texture(rhi::Device& device, std::uint32_t width,
                                           std::uint32_t height,
                                           const std::vector<std::uint8_t>& pixels) {
    rhi::TextureDesc desc;
    desc.extent = {width, height};
    desc.format = rhi::TextureFormat::R8G8B8A8Unorm;
    desc.pixels = pixels.data();
    desc.row_pitch = static_cast<std::size_t>(width) * 4;
    desc.filter = rhi::Filter::Nearest;
    desc.address = rhi::AddressMode::ClampToEdge;
    return device.create_texture(desc);
}

// Six procedural 8x8 glyph masks on transparency. Not a font: the point is that the text scene
// emits the same tinted textured quads every backend would emit for glyphs.
std::vector<std::uint8_t> glyph_atlas_pixels() {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kAtlasWidth) * kAtlasHeight * 4, 0);
    for (std::uint32_t glyph = 0; glyph < kGlyphCount; ++glyph) {
        for (std::uint32_t y = 0; y < kAtlasCell; ++y) {
            for (std::uint32_t x = 0; x < kAtlasCell; ++x) {
                const bool vertical_stem = x == 1U + glyph % 2U;
                const bool shoulder = y == 1U + (glyph * 2U) % 5U && x >= 2U && x <= 5U;
                const bool bowl = y >= 4U && y <= 6U && x >= 2U && x <= 5U;
                if (!vertical_stem && !shoulder && !bowl) {
                    continue;
                }
                const std::size_t index =
                    (static_cast<std::size_t>(y) * kAtlasWidth + glyph * kAtlasCell + x) * 4;
                pixels[index + 0] = 255;
                pixels[index + 1] = 255;
                pixels[index + 2] = 255;
                pixels[index + 3] = 255;
            }
        }
    }
    return pixels;
}

std::vector<std::uint8_t> checker_pixels() {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kTextureSide) * kTextureSide * 4, 0);
    for (std::uint32_t y = 0; y < kTextureSide; ++y) {
        for (std::uint32_t x = 0; x < kTextureSide; ++x) {
            const bool light = ((x / 8U) + (y / 8U)) % 2U == 0U;
            const std::size_t index = (static_cast<std::size_t>(y) * kTextureSide + x) * 4;
            pixels[index + 0] = light ? 226 : 40;
            pixels[index + 1] = light ? 232 : 52;
            pixels[index + 2] = light ? 240 : 72;
            pixels[index + 3] = 255;
        }
    }
    return pixels;
}

// A 16-wide horizontal ramp with a gentle vertical tint, sampled nearest at 3x. Every output pixel
// maps to one texel centre, so nearest sampling is exact and still exercises the texture path.
std::vector<std::uint8_t> ramp_pixels() {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(kTextureSide) * kTextureSide * 4, 0);
    for (std::uint32_t y = 0; y < kTextureSide; ++y) {
        for (std::uint32_t x = 0; x < kTextureSide; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * kTextureSide + x) * 4;
            pixels[index + 0] = static_cast<std::uint8_t>(32U + x * 14U);
            pixels[index + 1] = static_cast<std::uint8_t>(48U + y * 10U);
            pixels[index + 2] = static_cast<std::uint8_t>(96U + x * 8U);
            pixels[index + 3] = 255;
        }
    }
    return pixels;
}

void draw_solid_quad(rendering::gpu::Batched2DRenderer& renderer, const SceneTextures&) {
    renderer.push_rect({16.0f, 20.0f, 64.0f, 48.0f}, rhi::rgba8(220, 72, 72));
}

void draw_sdf_rounded_quad(rendering::gpu::Batched2DRenderer& renderer, const SceneTextures&) {
    renderer.push_rounded_rect({16.0f, 16.0f, 72.0f, 56.0f}, rhi::rgba8(56, 148, 214), 18.0f);
}

void draw_textured_quad(rendering::gpu::Batched2DRenderer& renderer,
                        const SceneTextures& textures) {
    // 1:1 checkerboard (each output pixel samples its own texel) plus a 3x nearest ramp.
    if (textures.checker != nullptr) {
        renderer.push_texture({16.0f, 16.0f, 16.0f, 16.0f}, *textures.checker,
                              {0.0f, 0.0f, 1.0f, 1.0f}, rhi::rgba8(255, 255, 255));
    }
    if (textures.ramp != nullptr) {
        renderer.push_texture({48.0f, 16.0f, 48.0f, 16.0f}, *textures.ramp,
                              {0.0f, 0.0f, 1.0f, 1.0f}, rhi::rgba8(255, 255, 255));
    }
}

void draw_clip(rendering::gpu::Batched2DRenderer& renderer, const SceneTextures&) {
    // A plain rectangular clip takes the scissor fast path, so the boundary is the fixed-function
    // scissor and not an SDF fragment test.
    renderer.push_clip(rendering::gpu::clip::uniform_rect({8.0f, 6.0f, 112.0f, 84.0f}, 0.0f));
    renderer.push_rect({0.0f, 0.0f, 128.0f, 96.0f}, kGold);
    renderer.pop_clip();
}

void draw_transform(rendering::gpu::Batched2DRenderer& renderer, const SceneTextures&) {
    renderer.set_transform(rhi::Mat3::translation(64.0f, 48.0f));
    renderer.push_transform(rhi::Mat3::rotation(0.3f));
    renderer.push_rect({-28.0f, -18.0f, 56.0f, 36.0f}, kCard);
    renderer.push_rounded_rect({-14.0f, -8.0f, 28.0f, 16.0f}, kAccent, 6.0f);
    renderer.pop_transform();
}

void draw_text(rendering::gpu::Batched2DRenderer& renderer, const SceneTextures& textures) {
    if (textures.glyphs == nullptr) {
        return;
    }
    for (std::uint32_t glyph = 0; glyph < kGlyphCount; ++glyph) {
        const float u = static_cast<float>(glyph) / static_cast<float>(kGlyphCount);
        const float width = 1.0f / static_cast<float>(kGlyphCount);
        renderer.push_texture({16.0f + static_cast<float>(glyph) * 10.0f, 40.0f, 8.0f, 8.0f},
                              *textures.glyphs, {u, 0.0f, width, 1.0f}, kInk);
    }
    renderer.push_rect({16.0f, 52.0f, 60.0f, 2.0f}, kAccent);
}

void draw_shadow(rendering::gpu::Batched2DRenderer& renderer, const SceneTextures&) {
    // Blur approximation: stacked translucent rounded layers, as the CPU sink does, so no extra
    // shader or offscreen target is involved in the parity comparison.
    for (std::uint32_t layer = 3; layer >= 1; --layer) {
        const float grow = static_cast<float>(layer) * 2.0f;
        renderer.push_rounded_rect(
            {24.0f - grow, 22.0f - grow, 80.0f + grow * 2.0f, 52.0f + grow * 2.0f},
            rhi::rgba8(0, 0, 0, 24), 14.0f + grow);
    }
    renderer.push_rounded_rect({24.0f, 22.0f, 80.0f, 52.0f}, kCard, 12.0f);
}

void draw_alpha_blend(rendering::gpu::Batched2DRenderer& renderer, const SceneTextures&) {
    renderer.push_rect({12.0f, 14.0f, 104.0f, 68.0f}, rhi::rgba8(24, 32, 48));
    renderer.push_rect({20.0f, 22.0f, 60.0f, 52.0f}, rhi::rgba8(220, 64, 64, 128));
    renderer.push_rect({52.0f, 36.0f, 56.0f, 44.0f}, rhi::rgba8(64, 120, 240, 110));
}

constexpr std::array<Scene, 8> kScenes = {{
    {"solid_quad", &draw_solid_quad},
    {"sdf_rounded_quad", &draw_sdf_rounded_quad},
    {"textured_quad", &draw_textured_quad},
    {"clip", &draw_clip},
    {"transform", &draw_transform},
    {"text", &draw_text},
    {"shadow", &draw_shadow},
    {"alpha_blend", &draw_alpha_blend},
}};

} // namespace

SceneTextures create_scene_textures(rhi::Device& device) {
    SceneTextures textures;
    textures.glyphs = make_texture(device, kAtlasWidth, kAtlasHeight, glyph_atlas_pixels());
    textures.checker = make_texture(device, kTextureSide, kTextureSide, checker_pixels());
    textures.ramp = make_texture(device, kTextureSide, kTextureSide, ramp_pixels());
    return textures;
}

std::span<const Scene> parity_scenes() {
    return kScenes;
}

rhi::Color parity_clear_color() noexcept {
    return rhi::rgba8(16, 20, 28);
}

} // namespace tessera::tests::gpu::backends
