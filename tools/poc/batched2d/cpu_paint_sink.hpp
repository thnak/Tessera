// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/image/image_decode_cache.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_scale.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/rendering/software/cpu_layer.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/assets/asset_image.hpp"
#include "tessera/ui/assets/asset_svg.hpp"
#include "tessera/ui/paint/gradient_fill.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/paint/vector_raster.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace tessera::poc::batched2d {

// CPU implementation of the paint sink used by `batched2d --backend cpu --cpu-text`.
// It reuses the paint pass output (TextDraw per line from PaintData) and the same FreeType glyph
// coverage bitmaps as the GPU text path, but composites them into the deterministic CPU canvas.
// Rounded rects and rounded clips are approximated by their axis-aligned bounds (the CPU golden
// scene uses plain rects); transforms are already folded into the paint rects.
//
// Isolated layers and layer masks (issue #617) are the CPU twin of the GPU path: while a layer is
// open every draw goes to a black-backed and a white-backed scratch canvas, and the pop composites
// the pair once (software/cpu_layer.hpp explains the recovery of alpha from the pair).
// Vector resources for the CPU sink. Borrowed; the caller owns the embedded bundle, the decoded
// document cache and the (CPU) path rasterizer for the lifetime of the sink. With any pointer null
// vector draws are a no-op, so CPU text-only scenes keep working unchanged.
struct CpuVectorConfig {
    const ui::assets::AssetBundle* bundle = nullptr;
    rendering::path::SvgDocumentCache* documents = nullptr;
    rendering::path::PathRasterizer* rasterizer = nullptr;
    float device_scale = 1.0f;
};

// Raster-image resources for the CPU sink. The decoded cache is borrowed and must outlive the sink.
// The CPU reference path blits the authored decode straight into the content box with nearest
// sampling (no intermediate resample), which equals the GPU result when the derived raster extent
// equals the content box (the deterministic golden case).
struct CpuImageConfig {
    const ui::assets::AssetBundle* bundle = nullptr;
    rendering::image::ImageDecodeCache* decoded = nullptr;
    float device_scale = 1.0f;
};

class CpuPaintSink final : public ui::paint::PaintSink {
public:
    CpuPaintSink(rendering::software::CpuCanvas& canvas,
                 ui::text::GlyphRasterizer* rasterizer = nullptr,
                 ui::text::TextShaper* shaper = nullptr, CpuVectorConfig vector = {},
                 CpuImageConfig image = {})
        : canvas_(&canvas),
          rasterizer_(rasterizer),
          shaper_(shaper),
          vector_(vector),
          image_(image),
          clip_{0.0f, 0.0f, static_cast<float>(canvas.width()),
                static_cast<float>(canvas.height())} {}

    void fill_rect(const rendering::rhi::Rect& rect, const rendering::rhi::Color& color) override {
        const rendering::rhi::Rect clipped = intersect(clip_, rect);
        if (clipped.width > 0.0f && clipped.height > 0.0f) {
            paint_each(
                [&](rendering::software::CpuCanvas& canvas) { canvas.fill_rect(clipped, color); });
        }
    }

    void fill_rounded_rect(const rendering::rhi::Rect& rect, const rendering::rhi::Color& color,
                           float /*radius*/) override {
        // The CPU golden scene does not use rounded rects; bounds-only approximation documented.
        fill_rect(rect, color);
    }

    // Gradients are evaluated per pixel, the CPU twin of the shader (docs/design/gpu/gradients.md,
    // ADR-076): the fill is packed into the same record the GPU reads (gradient_math.hpp) and
    // evaluated at every pixel centre with the shader's own expressions, so the two paths differ
    // only by the shader's arithmetic. Rounded corners are approximated by the rect bounds here
    // (the CPU golden path documents that approximation).
    void fill_linear_gradient(const ui::paint::GradientDraw& draw) override {
        const ui::paint::FillStops stops(draw.stops.data(), draw.stop_count);
        fill_gradient(ui::paint::linear_fill(draw, stops));
    }

    void fill_radial_gradient(const ui::paint::RadialGradientDraw& draw) override {
        const ui::paint::FillStops stops(draw.stops.data(), draw.stop_count);
        fill_gradient(ui::paint::radial_fill(draw, stops));
    }

    void fill_conic_gradient(const ui::paint::ConicGradientDraw& draw) override {
        const ui::paint::FillStops stops(draw.stops.data(), draw.stop_count);
        fill_gradient(ui::paint::conic_fill(draw, stops));
    }

    void push_clip(const ui::paint::RoundedClip& clip) override {
        stack_.push_back(clip_);
        clip_ = intersect(clip_, clip.rect);
    }

    void pop_clip() override {
        if (!stack_.empty()) {
            clip_ = stack_.back();
            stack_.pop_back();
        }
    }

    void draw_text(const ui::paint::TextDraw& run) override {
        if (run.text.empty() || rasterizer_ == nullptr || shaper_ == nullptr) {
            return;
        }
        ui::text::TextStyle style;
        style.font_size = run.font_size;
        style.font_weight = run.font_weight;
        const ui::text::ShapedRun shaped = shaper_->shape(run.text, style);
        if (shaped.glyphs.empty()) {
            return;
        }
        const float size = std::clamp(run.font_size, 1.0f, 4096.0f);
        const auto pixel_size = static_cast<std::uint16_t>(std::lround(size));
        const float baseline = run.baseline.value_or(run.rect.y + shaped.ascent);
        float pen_x = run.rect.x;
        for (const ui::text::Glyph& glyph : shaped.glyphs) {
            const ui::text::GlyphBitmap bitmap = rasterizer_->rasterize_glyph(glyph.id, pixel_size);
            if (!bitmap.coverage.empty()) {
                const auto left = static_cast<std::int32_t>(
                    std::lround(pen_x + glyph.x_offset + static_cast<float>(bitmap.bearing_x)));
                const auto top = static_cast<std::int32_t>(
                    std::lround(baseline - glyph.y_offset - static_cast<float>(bitmap.bearing_y)));
                paint_each([&](rendering::software::CpuCanvas& canvas) {
                    canvas.blend_coverage(left, top, bitmap.width, bitmap.height, bitmap.coverage,
                                          run.color, &clip_);
                });
            }
            pen_x += glyph.advance;
        }
    }

    // CPU vector paint: decode the embedded SVG -> rasterize the document shapes at the shared
    // derived size (vector_raster.hpp) through the CPU rasterizer -> integer textured-quad blit
    // with tint and the active clip. Missing/malformed assets and ThorVG-OFF builds are silent
    // no-ops, matching the GPU sink's contract.
    void draw_vector(const ui::paint::VectorDraw& draw) override {
        if (vector_.bundle == nullptr || vector_.documents == nullptr ||
            vector_.rasterizer == nullptr || draw.asset.empty()) {
            return;
        }
        const auto document =
            ui::assets::load_asset_svg(*vector_.bundle, draw.asset, *vector_.documents);
        if (!document.has_value()) {
            return;
        }
        const auto& decoded = **document;
        const auto request = ui::paint::resolve_vector_raster(draw, decoded.width, decoded.height,
                                                              vector_.device_scale);
        if (!request.has_value()) {
            return;
        }
        const std::size_t row_bytes = static_cast<std::size_t>(request->raster_width) * 4;
        std::vector<std::uint8_t> pixels(row_bytes * request->raster_height, 0);
        rendering::path::RasterTarget target{std::span<std::uint8_t>(pixels.data(), pixels.size()),
                                             request->raster_width, request->raster_height,
                                             static_cast<std::uint32_t>(row_bytes)};
        const float scale = static_cast<float>(request->scale_milli) / 1000.0f;
        const std::vector<rendering::path::PathShape> scaled =
            rendering::path::scale_path_shapes(decoded.shapes, scale);
        const std::span<const rendering::path::PathShape> to_rasterize =
            scaled.empty() ? std::span<const rendering::path::PathShape>(decoded.shapes)
                           : std::span<const rendering::path::PathShape>(scaled);
        const auto raster_start = std::chrono::steady_clock::now();
        const bool rasterized = vector_.rasterizer->rasterize(to_rasterize, target).has_value();
        raster_time_ += std::chrono::steady_clock::now() - raster_start;
        if (!rasterized) {
            return;
        }

        // The paint rect is fractional; the reference canvas works in whole pixels, so each edge
        // rounds half away from zero (deterministic for the golden scene).
        const auto round_edge = [](float value) {
            return static_cast<std::int32_t>(std::lround(value));
        };
        const std::int32_t left = round_edge(draw.rect.x);
        const std::int32_t top = round_edge(draw.rect.y);
        const std::int32_t right = round_edge(draw.rect.x + draw.rect.width);
        const std::int32_t bottom = round_edge(draw.rect.y + draw.rect.height);
        if (right <= left || bottom <= top) {
            return;
        }
        const auto blit_start = std::chrono::steady_clock::now();
        paint_each([&](rendering::software::CpuCanvas& canvas) {
            canvas.blit_texture(left, top, static_cast<std::uint32_t>(right - left),
                                static_cast<std::uint32_t>(bottom - top), pixels,
                                request->raster_width, request->raster_height, draw.tint, &clip_);
        });
        blit_time_ += std::chrono::steady_clock::now() - blit_start;
        ++vector_draws_;
    }

    // CPU raster-image paint: decode the embedded PNG/JPEG -> blit the authored RGBA8 pixels into
    // the integer content box with tint and the active clip. Missing/corrupt assets are no-ops,
    // matching the GPU sink's contract.
    void draw_image(const ui::paint::ImageDraw& draw) override {
        if (image_.bundle == nullptr || image_.decoded == nullptr || draw.asset.empty()) {
            return;
        }
        const auto decoded =
            ui::assets::load_asset_image(*image_.bundle, draw.asset, *image_.decoded);
        if (!decoded.has_value()) {
            return;
        }
        const auto request = ui::paint::resolve_image_raster(
            draw, (*decoded)->width, (*decoded)->height, image_.device_scale);
        if (!request.has_value()) {
            return;
        }
        const auto round_edge = [](float value) {
            return static_cast<std::int32_t>(std::lround(value));
        };
        const std::int32_t left = round_edge(draw.rect.x);
        const std::int32_t top = round_edge(draw.rect.y);
        const std::int32_t right = round_edge(draw.rect.x + draw.rect.width);
        const std::int32_t bottom = round_edge(draw.rect.y + draw.rect.height);
        if (right <= left || bottom <= top) {
            return;
        }
        paint_each([&](rendering::software::CpuCanvas& canvas) {
            canvas.blit_texture(left, top, static_cast<std::uint32_t>(right - left),
                                static_cast<std::uint32_t>(bottom - top), (*decoded)->rgba,
                                (*decoded)->width, (*decoded)->height, draw.tint, &clip_);
        });
    }

    // Glass backdrop: explicitly unsupported on the CPU reference sink (docs/specs/glass-v1.md
    // section 9). The deterministic canvas cannot read the frame back, and a sampled approximation
    // would be a golden no real backend could reproduce, so the node paints without the effect (the
    // stylesheet's opaque fallback) and the draw is counted as a diagnostic instead of silently
    // ignored.
    void draw_backdrop(const ui::paint::BackdropDraw& /*draw*/) override {
        ++glass_unsupported_;
    }

    [[nodiscard]] bool supports_layers() const override {
        return true;
    }

    void push_layer(const ui::paint::LayerDraw& layer) override {
        layers_.emplace_back(layer, canvas_->width(), canvas_->height());
    }

    // The group composites once, inside the layer's box and the clip that is active again by now
    // (the paint pass pops the node's clip before the layer), into whatever is open below it: the
    // frame, or an outer layer's pair, which stays consistent because the composite is linear.
    void pop_layer() override {
        if (layers_.empty()) {
            return;
        }
        Layer layer = std::move(layers_.back());
        layers_.pop_back();
        const rendering::rhi::Rect region = intersect(clip_, layer.box);
        paint_each([&](rendering::software::CpuCanvas& canvas) {
            rendering::software::composite_layer(canvas, layer.black, layer.white, region,
                                                 layer.opacity);
        });
    }

    // A layer takes one mask. The mask image is drawn into its own transparent pair, so seams and
    // overlaps are the ordinary blend's, and end_layer_mask() scales the layer by its alpha. No
    // open layer, or one that already took a mask, refuses like the GPU renderer does.
    [[nodiscard]] bool begin_layer_mask() override {
        if (layers_.empty() || layers_.back().masked) {
            return false;
        }
        layers_.back().begin_mask();
        return true;
    }

    void end_layer_mask() override {
        if (layers_.empty() || !layers_.back().mask_black.has_value()) {
            return;
        }
        Layer& layer = layers_.back();
        rendering::software::apply_layer_mask(layer.black, layer.white, *layer.mask_black,
                                              *layer.mask_white);
        layer.mask_black.reset();
        layer.mask_white.reset();
    }

private:
    // One gradient quad: pack once, then shade every covered pixel with the shared evaluator. The
    // quad-local point is the pixel centre minus the quad's centre, which is what the shader's
    // interpolated `v_local` is.
    void fill_gradient(const rendering::gpu::gradient::Fill& fill) {
        rendering::gpu::gradient::Packed packed;
        if (!rendering::gpu::gradient::pack(fill, packed)) {
            return;
        }
        const rendering::rhi::Rect clipped = intersect(clip_, fill.rect);
        if (clipped.width <= 0.0f || clipped.height <= 0.0f) {
            return;
        }
        const float centre_x = fill.rect.x + fill.rect.width * 0.5f;
        const float centre_y = fill.rect.y + fill.rect.height * 0.5f;
        paint_each([&](rendering::software::CpuCanvas& canvas) {
            canvas.fill_rect_shaded(clipped, [&](float x, float y) {
                return rendering::gpu::gradient::evaluate(packed, x - centre_x, y - centre_y);
            });
        });
    }

    // One open isolated layer: the content pair and, while a mask image is being drawn, the mask
    // pair. Frame-sized like the GPU's pooled target, so no coordinates shift.
    struct Layer {
        Layer(const ui::paint::LayerDraw& draw, std::uint32_t width, std::uint32_t height)
            : box(draw.box),
              opacity(draw.opacity),
              black(width, height),
              white(width, height) {
            black.clear(rendering::rhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
            white.clear(rendering::rhi::Color{1.0f, 1.0f, 1.0f, 1.0f});
        }

        void begin_mask() {
            masked = true;
            mask_black.emplace(black.width(), black.height());
            mask_white.emplace(black.width(), black.height());
            mask_black->clear(rendering::rhi::Color{0.0f, 0.0f, 0.0f, 1.0f});
            mask_white->clear(rendering::rhi::Color{1.0f, 1.0f, 1.0f, 1.0f});
        }

        rendering::rhi::Rect box;
        float opacity = 1.0f;
        rendering::software::CpuCanvas black;
        rendering::software::CpuCanvas white;
        std::optional<rendering::software::CpuCanvas> mask_black;
        std::optional<rendering::software::CpuCanvas> mask_white;
        bool masked = false;
    };

    // Runs `draw` on every canvas the next primitive lands on: the frame canvas, or the pair of the
    // innermost open layer (its mask pair while a mask image is being drawn).
    template<typename Draw> void paint_each(Draw&& draw) {
        if (layers_.empty()) {
            draw(*canvas_);
        } else if (Layer& layer = layers_.back(); layer.mask_black.has_value()) {
            draw(*layer.mask_black);
            draw(*layer.mask_white);
        } else {
            draw(layer.black);
            draw(layer.white);
        }
    }

    static rendering::rhi::Rect intersect(const rendering::rhi::Rect& lhs,
                                          const rendering::rhi::Rect& rhs) {
        const float left = std::max(lhs.x, rhs.x);
        const float top = std::max(lhs.y, rhs.y);
        const float right = std::min(lhs.x + lhs.width, rhs.x + rhs.width);
        const float bottom = std::min(lhs.y + lhs.height, rhs.y + rhs.height);
        if (left >= right || top >= bottom) {
            return {0.0f, 0.0f, 0.0f, 0.0f};
        }
        return {left, top, right - left, bottom - top};
    }

    rendering::software::CpuCanvas* canvas_ = nullptr;
    ui::text::GlyphRasterizer* rasterizer_ = nullptr;
    ui::text::TextShaper* shaper_ = nullptr;
    CpuVectorConfig vector_;
    CpuImageConfig image_;
    rendering::rhi::Rect clip_;
    std::vector<rendering::rhi::Rect> stack_;
    std::vector<Layer> layers_;
    std::chrono::steady_clock::duration raster_time_{};
    std::chrono::steady_clock::duration blit_time_{};
    std::uint32_t vector_draws_ = 0;
    std::uint32_t glass_unsupported_ = 0;

public:
    // Diagnostics for the CPU golden/measurement path only; pixels never depend on these.
    [[nodiscard]] double raster_ms() const {
        return std::chrono::duration<double, std::milli>(raster_time_).count();
    }
    [[nodiscard]] double blit_ms() const {
        return std::chrono::duration<double, std::milli>(blit_time_).count();
    }
    [[nodiscard]] std::uint32_t vector_draws() const {
        return vector_draws_;
    }
    // Glass backdrop draws this sink reported as unsupported (never painted).
    [[nodiscard]] std::uint32_t glass_unsupported() const {
        return glass_unsupported_;
    }
};

} // namespace tessera::poc::batched2d
