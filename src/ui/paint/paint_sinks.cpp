// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/path/path_texture_key.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/path/svg_loader.hpp"
#include "tessera/rendering/path/svg_texture.hpp"
#include "tessera/ui/assets/asset_image.hpp"
#include "tessera/ui/assets/asset_svg.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
#include "tessera/ui/paint/vector_raster.hpp"
#include "tessera/ui/text/text_renderer.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace tessera::ui::paint {
namespace {

constexpr float kMinPixelSize = 1.0f;
constexpr float kMaxPixelSize = 4096.0f;
// The SVG decode API takes std::uint8_t spans; the bundle stores std::byte. char-like types may
// alias any object, so this is a view cast, not a copy.
[[nodiscard]] std::span<const std::uint8_t> as_u8(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

} // namespace

Batched2DSink::Batched2DSink(rendering::gpu::Batched2DRenderer& renderer,
                             text::TextRenderer* text_renderer, text::TextShaper* shaper,
                             VectorPaintConfig vector, ImagePaintConfig image)
    : renderer_(&renderer),
      text_renderer_(text_renderer),
      shaper_(shaper),
      vector_(vector),
      image_(image) {}

void Batched2DSink::fill_rect(const rhi::Rect& rect, const rhi::Color& color) {
    renderer_->push_rect(rect, color);
}

void Batched2DSink::fill_rounded_rect(const rhi::Rect& rect, const rhi::Color& color,
                                      float radius) {
    renderer_->push_rounded_rect(rect, color, radius);
}

void Batched2DSink::push_clip(const RoundedClip& clip) {
    renderer_->push_clip(rendering::gpu::clip::RoundedRect{clip.rect, clip.top_left, clip.top_right,
                                                           clip.bottom_right, clip.bottom_left});
}

void Batched2DSink::pop_clip() {
    renderer_->pop_clip();
}

void Batched2DSink::begin_text_frame() {
    if (text_renderer_ != nullptr) {
        text_renderer_->begin_frame();
    }
}

void Batched2DSink::draw_text(const TextDraw& run) {
    if (run.text.empty()) {
        return;
    }
    if (text_renderer_ == nullptr || shaper_ == nullptr) {
        throw std::runtime_error(
            "Batched2DSink::draw_text: no TextRenderer/TextShaper configured for this sink");
    }

    text::TextStyle style;
    style.font_size = run.font_size;
    style.font_weight = run.font_weight;
    const text::ShapedRun shaped = shaper_->shape(run.text, style);
    if (shaped.glyphs.empty()) {
        return;
    }

    const float size = std::clamp(run.font_size, kMinPixelSize, kMaxPixelSize);
    const auto pixel_size = static_cast<std::uint16_t>(std::lround(size));
    // Wrapped lines carry an absolute baseline from the shared line-box layout; single-line runs
    // keep the previous top-aligned behavior.
    const float baseline = run.baseline.value_or(run.rect.y + shaped.ascent);
    // TextRenderer pushes textured quads through the same renderer: a texture change flushes the
    // current rect batch, and the active scissor/rounded-clip state applies to the glyphs.
    text_renderer_->draw(*renderer_, shaped, run.rect.x, baseline, pixel_size, run.color);
}

void Batched2DSink::draw_vector(const VectorDraw& draw) {
    if (draw.asset.empty() || draw.rect.width <= 0.0f || draw.rect.height <= 0.0f) {
        return;
    }
    if (vector_.bundle == nullptr || vector_.documents == nullptr || vector_.textures == nullptr) {
        // Documented no-op: a sink without vector resources (or a ThorVG-OFF build) paints the
        // node's box visuals only.
        ++vector_stats_.missing_resources;
        return;
    }

    const auto asset = vector_.bundle->find(draw.asset);
    if (!asset.has_value()) {
        ++vector_stats_.missing_assets;
        return;
    }

    rhi::Color tint = draw.tint;
    tint.a = std::clamp(tint.a, 0.0f, 1.0f);
    if (tint.a <= 0.0f) {
        return;
    }

    // Decode once per asset content (keyed by hash inside SvgDocumentCache).
    const auto document =
        ui::assets::load_asset_svg(*vector_.bundle, draw.asset, *vector_.documents);
    if (!document.has_value()) {
        switch (document.error()) {
        case ui::assets::AssetSvgError::not_found:
        case ui::assets::AssetSvgError::invalid_name:
            ++vector_stats_.missing_assets;
            break;
        case ui::assets::AssetSvgError::backend_unavailable:
            ++vector_stats_.unavailable_backend;
            break;
        default:
            ++vector_stats_.failed_assets;
            break;
        }
        return;
    }

    const auto& decoded = **document;
    // One size/scale rule for GPU and CPU sinks (vector_raster.hpp): derived or explicit
    // device-pixel raster extents plus the uniform scale_milli. Nullopt means a degenerate request
    // -> no-op.
    const auto request =
        resolve_vector_raster(draw, decoded.width, decoded.height, vector_.device_scale);
    if (!request.has_value()) {
        return;
    }

    // One texture per (document content, raster scale).
    const rendering::path::SvgDocumentKey document_key =
        rendering::path::make_svg_document_key(as_u8(asset->bytes));
    const rendering::path::SvgTextureKey texture_key = rendering::path::make_svg_texture_key(
        document_key.content_hash, 0, decoded, request->scale_milli);
    const auto texture =
        rendering::path::acquire_svg_texture(*vector_.textures, texture_key, decoded);
    if (!texture.has_value()) {
        ++vector_stats_.failed_assets;
        return;
    }
    renderer_->push_texture(draw.rect, *texture->texture, {0.0f, 0.0f, 1.0f, 1.0f}, tint);
    ++vector_stats_.draws;
}

void Batched2DSink::draw_image(const ImageDraw& draw) {
    if (draw.asset.empty() || draw.rect.width <= 0.0f || draw.rect.height <= 0.0f) {
        return;
    }
    if (image_.bundle == nullptr || image_.decoded == nullptr || image_.textures == nullptr) {
        // Documented no-op: a sink without image resources paints the node's box visuals only.
        ++image_stats_.missing_resources;
        return;
    }
    const auto decoded =
        ui::assets::load_asset_image(*image_.bundle, draw.asset, *image_.decoded, image_.limits);
    if (!decoded.has_value()) {
        switch (decoded.error()) {
        case ui::assets::AssetImageError::not_found:
        case ui::assets::AssetImageError::invalid_name:
            ++image_stats_.missing_assets;
            break;
        default:
            ++image_stats_.corrupted_assets;
            break;
        }
        return;
    }
    const auto request =
        resolve_image_raster(draw, (*decoded)->width, (*decoded)->height, image_.device_scale);
    if (!request.has_value()) {
        return;
    }
    rhi::Color tint = draw.tint;
    tint.a = std::clamp(tint.a, 0.0f, 1.0f);
    if (tint.a <= 0.0f) {
        return;
    }

    // One texture per (content hash, raster extent, format). The extent resample is deterministic
    // (nearest), so the uploaded bytes are a function of the decode and the key.
    const rendering::image::ImageTextureKey texture_key{
        (*decoded)->content_hash, request->raster_width, request->raster_height,
        image_.texture_format};
    const auto texture = image_.textures->acquire(**decoded, texture_key);
    if (!texture.has_value()) {
        ++image_stats_.texture_failures;
        return;
    }
    renderer_->push_texture(draw.rect, *texture->texture, {0.0f, 0.0f, 1.0f, 1.0f}, tint);
    ++image_stats_.draws;
}

} // namespace tessera::ui::paint
