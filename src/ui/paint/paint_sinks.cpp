// SPDX-License-Identifier: MIT

#include "image_filter.hpp"
#include "tessera/rendering/gpu/batched2d_renderer.hpp"
#include "tessera/rendering/image/image_resize.hpp"
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

void Batched2DSink::fill_rounded_ring(const rhi::Rect& rect, const rhi::Color& color, float radius,
                                      float width) {
    renderer_->push_rounded_ring(rect, color, radius, width);
}

void Batched2DSink::fill_box_shadow(const BoxShadowDraw& draw) {
    if (draw.inset) {
        renderer_->push_inset_shadow(draw.box, draw.carve.rect, draw.color, draw.radius,
                                     draw.sigma);
        return;
    }
    renderer_->push_soft_shadow(draw.box, draw.carve.rect, draw.color, draw.radius, draw.sigma);
}

void Batched2DSink::push_clip(const RoundedClip& clip) {
    renderer_->push_clip(rendering::gpu::clip::RoundedRect{clip.rect, clip.top_left, clip.top_right,
                                                           clip.bottom_right, clip.bottom_left});
}

void Batched2DSink::pop_clip() {
    renderer_->pop_clip();
}

void Batched2DSink::push_transform(const rhi::Mat3& transform) {
    renderer_->push_transform(transform);
}

void Batched2DSink::pop_transform() {
    renderer_->pop_transform();
}

void Batched2DSink::begin_text_frame() {
    if (text_renderer_ != nullptr) {
        text_renderer_->begin_frame();
    }
}

bool Batched2DSink::supports_backdrop() const {
    return renderer_->glass_supported();
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
    style.features = run.features;
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
    text_renderer_->draw(*renderer_, shaped, run.rect.x, baseline, pixel_size, run.color, run.lcd);
}

void Batched2DSink::draw_vector(const VectorDraw& draw) {
    if (draw.asset.empty() || draw.rect.width <= 0.0f || draw.rect.height <= 0.0f) {
        return;
    }
    if ((vector_.bundles == nullptr && vector_.bundle == nullptr) || vector_.documents == nullptr ||
        vector_.textures == nullptr) {
        // Documented no-op: a sink without vector resources (or a ThorVG-OFF build) paints the
        // node's box visuals only.
        ++vector_stats_.missing_resources;
        renderer_->note_layer_degraded();
        return;
    }

    // A configured composition is the whole lookup source; the single bundle stays the default for
    // one-bundle callers. Both are allocation-free value lookups over read-only tables.
    const auto asset = vector_.bundles != nullptr ? vector_.bundles->find(draw.asset)
                                                  : vector_.bundle->find(draw.asset);
    if (!asset.has_value()) {
        ++vector_stats_.missing_assets;
        renderer_->note_layer_degraded();
        return;
    }

    rhi::Color tint = draw.tint;
    tint.a = std::clamp(tint.a, 0.0f, 1.0f);
    if (tint.a <= 0.0f) {
        return;
    }

    // Decode once per asset content (keyed by hash inside SvgDocumentCache).
    const auto document =
        vector_.bundles != nullptr
            ? ui::assets::load_asset_svg(*vector_.bundles, draw.asset, *vector_.documents)
            : ui::assets::load_asset_svg(*vector_.bundle, draw.asset, *vector_.documents);
    if (!document.has_value()) {
        switch (document.error()) {
        case ui::assets::AssetSvgError::not_found:
        case ui::assets::AssetSvgError::invalid_name:
            ++vector_stats_.missing_assets;
            renderer_->note_layer_degraded();
            break;
        case ui::assets::AssetSvgError::backend_unavailable:
            ++vector_stats_.unavailable_backend;
            renderer_->note_layer_degraded();
            break;
        default:
            ++vector_stats_.failed_assets;
            renderer_->note_layer_degraded();
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
        renderer_->note_layer_degraded();
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
        renderer_->note_layer_degraded();
        return;
    }
    // Decode at the raster extent the draw will upload (issue #14): the explicit extents, else the
    // content box x scale. One extent given means the other is unknown, so the box is square.
    const auto decoded =
        ui::assets::load_asset_image(*image_.bundle, draw.asset, *image_.decoded, image_.limits,
                                     image_decode_target(draw, image_.device_scale));
    if (!decoded.has_value()) {
        switch (decoded.error()) {
        case ui::assets::AssetImageError::not_found:
        case ui::assets::AssetImageError::invalid_name:
            ++image_stats_.missing_assets;
            renderer_->note_layer_degraded();
            break;
        default:
            ++image_stats_.corrupted_assets;
            renderer_->note_layer_degraded();
            break;
        }
        return;
    }
    const auto request = resolve_image_raster(draw, (*decoded)->source_width,
                                              (*decoded)->source_height, image_.device_scale);
    if (!request.has_value()) {
        return;
    }
    rhi::Color tint = draw.tint;
    tint.a = std::clamp(tint.a, 0.0f, 1.0f);
    if (tint.a <= 0.0f) {
        return;
    }

    // The texture holds the decoded pixels at their own extent (#682): one upload per (content
    // hash, decoded extent, format, filter, levels), whatever raster extent a draw wants, so a
    // resize or a zoom inside a decode bucket re-uploads nothing and the CPU never resamples. The
    // draw maps the decoded pixels to the raster on the GPU.
    //
    // S8 rule, unchanged: a draw aligned 1:1 with the raster extent in device space is the
    // deterministic reference and samples nearest; every other draw samples linear. The aligned
    // draw goes through `push_texture_exact`, which applies the CPU resample rule in the shader, so
    // its pixels equal the old "resample on the CPU, then draw 1:1" bytes at every ratio (the
    // hardware nearest sampler would not: it picks the wrong neighbor on every pixel of a 2:1
    // downscale). A non-aligned draw samples the decoded texture linearly, with a mip chain when it
    // minifies (RHI 1.6); the key carries the level count because the sampler's LOD range is fixed
    // at creation. `device_scale` is the factor that chose the raster extent, so the comparison is
    // against the pixels the draw actually covers (the frame's adaptive render-scale factor is not
    // visible to the sink, exactly as it is not visible to the raster size rule).
    const float device_scale = image_.device_scale;
    const rhi::Rect device_rect{draw.rect.x * device_scale, draw.rect.y * device_scale,
                                draw.rect.width * device_scale, draw.rect.height * device_scale};
    const rhi::Extent2D raster{request->raster_width, request->raster_height};
    const rhi::Filter filter =
        image_filter_for_draw(device_rect, raster, device_rect.x, device_rect.y);
    const bool exact = filter == rhi::Filter::Nearest;
    const bool mips = image_.textures->supports_mipmaps();
    const rhi::Extent2D source{(*decoded)->width, (*decoded)->height};
    const std::uint32_t levels = exact ? 1U : image_levels_for_draw(device_rect, source, mips);
    auto texture = image_.textures->acquire(
        **decoded,
        rendering::image::ImageTextureKey{(*decoded)->content_hash, source.width, source.height,
                                          image_.texture_format, filter, levels});
    if (!texture.has_value() &&
        texture.error() == rendering::image::ImageTextureError::entry_too_large) {
        // A decode too big to keep (a huge original drawn small) falls back to the old route: a
        // raster-sized texture, resampled once on the CPU, which fits where the original does not.
        texture = image_.textures->acquire(
            **decoded,
            rendering::image::ImageTextureKey{
                (*decoded)->content_hash, raster.width, raster.height, image_.texture_format,
                filter, exact ? 1U : image_levels_for_draw(device_rect, raster, mips)});
    }
    if (!texture.has_value()) {
        ++image_stats_.texture_failures;
        renderer_->note_layer_degraded();
        return;
    }
    if (exact) {
        renderer_->push_texture_exact(draw.rect, *texture->texture, tint);
    } else {
        renderer_->push_texture(draw.rect, *texture->texture, {0.0f, 0.0f, 1.0f, 1.0f}, tint);
    }
    ++image_stats_.draws;
}

bool Batched2DSink::supports_layers() const {
    return renderer_->layers_supported();
}

void Batched2DSink::push_layer(const LayerDraw& layer) {
    renderer_->push_layer(layer.box, layer.opacity, layer.filter);
}

void Batched2DSink::pop_layer() {
    renderer_->pop_layer();
}

bool Batched2DSink::retain_layer(const LayerRetention& retention) {
    return renderer_->retain_layer(retention.owner, retention.key, retention.clean);
}

bool Batched2DSink::begin_layer_mask() {
    return renderer_->begin_layer_mask();
}

void Batched2DSink::end_layer_mask() {
    renderer_->end_layer_mask();
}

bool Batched2DSink::begin_mask_layer(rendering::gpu::MaskCompositeOp op) {
    return renderer_->begin_mask_layer(op);
}

void Batched2DSink::end_mask_layer() {
    renderer_->end_mask_layer();
}

void Batched2DSink::draw_backdrop(const BackdropDraw& draw) {
    // The paint pass segments the frame at this call (glass-v1 section 4): everything before the
    // glass node is already recorded, so the renderer can break the pass here and read what the
    // frame holds. The renderer records copy -> horizontal blur -> vertical blur + composite on the
    // frame's own command list (see Batched2DRenderer::draw_backdrop), so this call never blocks.
    rendering::gpu::GlassBackdropDraw glass;
    glass.clip =
        rendering::gpu::clip::RoundedRect{draw.clip.rect, draw.clip.top_left, draw.clip.top_right,
                                          draw.clip.bottom_right, draw.clip.bottom_left};
    glass.sigma = draw.sigma;
    glass.opacity = draw.opacity;
    glass.saturate = draw.saturate;
    glass.brightness = draw.brightness;
    glass.contrast = draw.contrast;
    if (renderer_->draw_backdrop(glass)) {
        ++glass_stats_.draws;
        return;
    }
    // Bounded fallback (glass-v1 section 8): no glass pipelines, a frame target that cannot be
    // read, or the pool having no free target this frame. The node paints without the effect and
    // the draw is counted so a frame that silently paints no glass stays attributable.
    ++glass_stats_.unsupported;
    renderer_->note_layer_degraded();
}

} // namespace tessera::ui::paint
