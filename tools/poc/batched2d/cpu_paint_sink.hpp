// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/image/image_decode_cache.hpp"
#include "tessera/rendering/path/path_rasterizer.hpp"
#include "tessera/rendering/path/path_scale.hpp"
#include "tessera/rendering/path/svg_document_cache.hpp"
#include "tessera/rendering/software/cpu_canvas.hpp"
#include "tessera/ui/assets/asset_bundle.hpp"
#include "tessera/ui/assets/asset_image.hpp"
#include "tessera/ui/assets/asset_svg.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/paint/vector_raster.hpp"
#include "tessera/ui/text/glyph_raster.hpp"
#include "tessera/ui/text/text_shaper.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <span>
#include <vector>

namespace tessera::poc::batched2d {

// CPU implementation of the paint sink used by `batched2d --backend cpu --cpu-text`.
// It reuses the paint pass output (TextDraw per line from PaintData) and the same FreeType glyph
// coverage bitmaps as the GPU text path, but composites them into the deterministic CPU canvas.
// Rounded rects and rounded clips are approximated by their axis-aligned bounds (the CPU golden
// scene uses plain rects); transforms are already folded into the paint rects.
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
            canvas_->fill_rect(clipped, color);
        }
    }

    void fill_rounded_rect(const rendering::rhi::Rect& rect, const rendering::rhi::Color& color,
                           float /*radius*/) override {
        // The CPU golden scene does not use rounded rects; bounds-only approximation documented.
        fill_rect(rect, color);
    }

    // V1 gradient parity: the same 64 dominant-axis bands the GPU sink emits, sampled with the
    // shared encoded-space ramp. Rounded corners are approximated by rect bounds here (the CPU
    // golden path documents that approximation), so parity is exact for unrounded gradients.
    void fill_linear_gradient(const ui::paint::GradientDraw& draw) override {
        if (draw.stop_count == 0 || draw.rect.width <= 0.0f || draw.rect.height <= 0.0f) {
            return;
        }
        const float radians = draw.angle_degrees * 3.14159265358979323846f / 180.0f;
        const float dx = std::sin(radians);
        const float dy = -std::cos(radians);
        const float span_x = draw.rect.width * std::abs(dx);
        const float span_y = draw.rect.height * std::abs(dy);
        const float span = std::max(span_x + span_y, 1.0f);
        constexpr int kBands = 64;
        if (std::abs(dx) >= std::abs(dy)) {
            const float band = draw.rect.width / static_cast<float>(kBands);
            for (int index = 0; index < kBands; ++index) {
                const float x = draw.rect.x + (static_cast<float>(index) + 0.5f) * band;
                const float t = 0.5f + ((x - (draw.rect.x + draw.rect.width * 0.5f)) * dx) / span;
                fill_rect({x - band * 0.5f, draw.rect.y, band + 0.5f, draw.rect.height},
                          ui::paint::sample_linear_gradient(draw, t));
            }
        } else {
            const float band = draw.rect.height / static_cast<float>(kBands);
            for (int index = 0; index < kBands; ++index) {
                const float y = draw.rect.y + (static_cast<float>(index) + 0.5f) * band;
                const float t = 0.5f + ((y - (draw.rect.y + draw.rect.height * 0.5f)) * dy) / span;
                fill_rect({draw.rect.x, y - band * 0.5f, draw.rect.width, band + 0.5f},
                          ui::paint::sample_linear_gradient(draw, t));
            }
        }
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
                canvas_->blend_coverage(left, top, bitmap.width, bitmap.height, bitmap.coverage,
                                        run.color, &clip_);
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
        canvas_->blit_texture(left, top, static_cast<std::uint32_t>(right - left),
                              static_cast<std::uint32_t>(bottom - top), pixels,
                              request->raster_width, request->raster_height, draw.tint, &clip_);
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
        canvas_->blit_texture(left, top, static_cast<std::uint32_t>(right - left),
                              static_cast<std::uint32_t>(bottom - top), (*decoded)->rgba,
                              (*decoded)->width, (*decoded)->height, draw.tint, &clip_);
    }

    // Glass backdrop: explicitly unsupported on the CPU reference sink (docs/specs/glass-v1.md
    // section 9). The deterministic canvas cannot read the frame back, and a sampled approximation
    // would be a golden no real backend could reproduce, so the node paints without the effect (the
    // stylesheet's opaque fallback) and the draw is counted as a diagnostic instead of silently
    // ignored.
    void draw_backdrop(const ui::paint::BackdropDraw& /*draw*/) override {
        ++glass_unsupported_;
    }

private:
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
