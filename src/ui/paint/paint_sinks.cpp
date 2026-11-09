// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/path/path_texture_key.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/path/svg_loader.hpp"
#include "tessera/rendering/path/svg_texture.hpp"
#include "tessera/ui/assets/asset_svg.hpp"
#include "tessera/ui/paint/batched2d_sink.hpp"
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
constexpr float kMinVectorScale = 0.01f;
constexpr float kMaxVectorScale = 64.0f;
constexpr std::uint32_t kMaxRasterExtent = 16384;

// The SVG decode API takes std::uint8_t spans; the bundle stores std::byte. char-like types may
// alias any object, so this is a view cast, not a copy.
[[nodiscard]] std::span<const std::uint8_t> as_u8(std::span<const std::byte> bytes) noexcept {
    return {reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()};
}

[[nodiscard]] std::uint32_t clamp_raster_extent(double value) {
    if (!(value > 0.0)) {
        return 1;
    }
    const auto rounded = static_cast<std::uint32_t>(std::llround(value));
    return std::clamp<std::uint32_t>(rounded, 1, kMaxRasterExtent);
}

[[nodiscard]] std::uint32_t scale_milli_from(double raster_px, double document_px) {
    if (!(document_px > 0.0)) {
        return 1000;
    }
    const double milli = 1000.0 * raster_px / document_px;
    return static_cast<std::uint32_t>(std::clamp<double>(std::llround(milli), 1.0, 16000.0));
}

} // namespace

Batched2DSink::Batched2DSink(rendering::gpu::Batched2DRenderer& renderer,
                             text::TextRenderer* text_renderer, text::TextShaper* shaper,
                             VectorPaintConfig vector)
    : renderer_(&renderer),
      text_renderer_(text_renderer),
      shaper_(shaper),
      vector_(vector) {}

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

    const float requested_scale = draw.scale > 0.0f ? draw.scale : vector_.device_scale;
    if (!(requested_scale > 0.0f) || !std::isfinite(requested_scale)) {
        return;
    }
    const float scale = std::clamp(requested_scale, kMinVectorScale, kMaxVectorScale);
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
    const double document_width = std::max(1.0, static_cast<double>(decoded.width));
    const double document_height = std::max(1.0, static_cast<double>(decoded.height));
    std::uint32_t raster_width = draw.raster_width;
    std::uint32_t raster_height = draw.raster_height;
    if (raster_width == 0U && raster_height == 0U) {
        raster_width = clamp_raster_extent(static_cast<double>(draw.rect.width) * scale);
        raster_height = clamp_raster_extent(static_cast<double>(draw.rect.height) * scale);
    } else if (raster_width == 0U) {
        raster_width = clamp_raster_extent(static_cast<double>(raster_height) * document_width /
                                           document_height);
    } else if (raster_height == 0U) {
        raster_height = clamp_raster_extent(static_cast<double>(raster_width) * document_height /
                                            document_width);
    }
    raster_width = std::clamp<std::uint32_t>(raster_width, 1, kMaxRasterExtent);
    raster_height = std::clamp<std::uint32_t>(raster_height, 1, kMaxRasterExtent);

    // One texture per (document content, raster scale). The scale is uniform and derived from the
    // document width; an explicit raster size that ignores the document aspect stretches the
    // texture when drawn into `rect` (documented).
    const rendering::path::SvgDocumentKey document_key =
        rendering::path::make_svg_document_key(as_u8(asset->bytes));
    const rendering::path::SvgTextureKey texture_key = rendering::path::make_svg_texture_key(
        document_key.content_hash, 0, decoded, scale_milli_from(raster_width, document_width));
    const auto texture =
        rendering::path::acquire_svg_texture(*vector_.textures, texture_key, decoded);
    if (!texture.has_value()) {
        ++vector_stats_.failed_assets;
        return;
    }
    renderer_->push_texture(draw.rect, *texture->texture, {0.0f, 0.0f, 1.0f, 1.0f}, tint);
    ++vector_stats_.draws;
}

} // namespace tessera::ui::paint
