// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip_records.hpp"
#include "tessera/rendering/rhi/types.hpp"

#include <cstdint>
#include <span>
#include <vector>
namespace tessera::rendering::software {

// A shaped clip for the CPU reference (ADR-068 slice 4): a list of clip records the canvas tests
// with `gpu::clip::covers_pixel`, the same rule the fragment shader's hard-test path implements.
// Keeping the rule in one function is what makes a shaped-clip golden a test rather than a
// tolerance argument.
//
// A null `ShapedClip*` means "no shaped clip", which is the axis-aligned `rhi::Rect` path every
// existing caller uses. A non-null one with an empty record list clips everything away, which is
// the honest reading of an empty intersection.
struct ShapedClip {
    std::span<const gpu::clip::ClipRecord> records;
    // The vertex pool the `Polygon` records index (ADR-068 slice 5); empty when none is a polygon.
    std::span<const gpu::clip::ClipVertex> vertices = {};

    [[nodiscard]] bool empty() const {
        return records.empty();
    }
};

// Minimal deterministic CPU canvas for golden references. It fills axis-aligned solid
// rectangles with hard pixel edges and, additively, composites 8-bit glyph coverage bitmaps for
// the CPU text golden. It is not a general vector rasterizer; the lavapipe path remains the
// full-fidelity software renderer.
class CpuCanvas {
public:
    CpuCanvas(std::uint32_t width, std::uint32_t height);

    void clear(rhi::Color color);
    void fill_rect(const rhi::Rect& rect, rhi::Color color);

    // Fills `rect` with `color`, keeping only pixels the shaped clip covers. This is the CPU
    // reference for a shaped `clip-path`: the coverage rule is `gpu::clip::covers_pixel`, so the
    // result is what the shader's hard-test path produces for the same record.
    void fill_rect_clipped(const rhi::Rect& rect, rhi::Color color, const ShapedClip& clip);

    // Fills `rect` with one colour per pixel: `shade(centre_x, centre_y)` is called with each pixel
    // centre that fill_rect would cover (same pixel-centre rule) and returns the straight-alpha
    // colour to blend there. This is the CPU reference of a per-fragment shader - the gradient quad
    // (docs/design/gpu/gradients.md) - so it blends in float exactly like a solid fill does and a
    // shader's output and this one differ only by the shader's own arithmetic. `Shade` is a
    // callable `rhi::Color(float, float)`; nothing allocates.
    template<typename Shade> void fill_rect_shaded(const rhi::Rect& rect, Shade&& shade) {
        const PixelSpan span = pixel_span(rect);
        for (std::uint32_t y = span.first_y; y < span.last_y && y < height_; ++y) {
            const float center_y = static_cast<float>(y) + 0.5f;
            if (center_y < span.top || center_y >= span.bottom) {
                continue;
            }
            for (std::uint32_t x = span.first_x; x < span.last_x && x < width_; ++x) {
                const float center_x = static_cast<float>(x) + 0.5f;
                if (center_x < span.left || center_x >= span.right) {
                    continue;
                }
                const rhi::Color color = shade(center_x, center_y);
                if (color.a > 0.0f) {
                    blend(x, y, color);
                }
            }
        }
    }

    // Deterministic reference text blend: composites one 8-bit coverage bitmap (row-major, one
    // byte per pixel, `width * height` bytes) with straight-alpha source-over into an opaque
    // framebuffer. `x`/`y` are the integer bitmap top-left; pixels outside the canvas or outside
    // `clip` (pixel-center test, same rule as fill_rect) are dropped. Integer rounding:
    //   src channel  = round(color.channel * 255)
    //   alpha8       = round(color.a * 255)
    //   pixel alpha  = (alpha8 * coverage + 127) / 255
    //   out channel  = (src * pixel_alpha + dst * (255 - pixel_alpha) + 127) / 255
    // `coverage == 0` leaves the destination untouched, `coverage == 255` is a plain source-over.
    // The GPU path blends in float before UNORM conversion, so a GPU-vs-CPU pixel may differ by up
    // to 1 LSB; the CPU result is the software-golden reference.
    void blend_coverage(std::int32_t x, std::int32_t y, std::uint32_t width, std::uint32_t height,
                        std::span<const std::uint8_t> coverage, rhi::Color color,
                        const rhi::Rect* clip = nullptr);

    // Deterministic reference textured-quad blit: composites one straight-alpha RGBA8 bitmap
    // (row-major, 4 bytes per pixel, `texture_width * texture_height * 4` bytes) into the opaque
    // framebuffer as a `width` x `height` destination quad at (x, y). Nearest sampling with a
    // documented pixel-center rule:
    //   source column = floor((dx + 0.5) * texture_width / width), clamped to texture_width - 1
    //   source row    = floor((dy + 0.5) * texture_height / height), clamped to texture_height - 1
    // Tint multiplies every sampled channel and the sampled alpha, then straight-alpha source-over
    // uses the canvas's existing integer rounding:
    //   src channel  = (sampled channel * round(tint.channel * 255) + 127) / 255
    //   pixel alpha  = (sampled alpha * round(tint.a * 255) + 127) / 255
    //   out channel  = (src * pixel_alpha + dst * (255 - pixel_alpha) + 127) / 255
    // `pixel_alpha == 0` leaves the destination untouched, `255` writes src directly, and the
    // framebuffer alpha stays 255. Pixels outside the canvas or outside `clip` (pixel-center test,
    // same rule as fill_rect) are dropped. This is the whole additive capability needed for a
    // deterministic vector-paint golden; it is not a general rasterizer or image filter.
    void blit_texture(std::int32_t x, std::int32_t y, std::uint32_t width, std::uint32_t height,
                      std::span<const std::uint8_t> rgba, std::uint32_t texture_width,
                      std::uint32_t texture_height, rhi::Color tint,
                      const rhi::Rect* clip = nullptr);

    [[nodiscard]] std::uint32_t width() const {
        return width_;
    }
    [[nodiscard]] std::uint32_t height() const {
        return height_;
    }
    [[nodiscard]] std::span<const std::uint8_t> pixels() const {
        return pixels_;
    }
    [[nodiscard]] std::span<std::uint8_t> pixels() {
        return pixels_;
    }

private:
    // The pixels a rect can reach, clamped to the canvas: the integer range to scan and the float
    // bounds the pixel-centre test compares against. An empty span has first >= last.
    struct PixelSpan {
        std::uint32_t first_x = 0;
        std::uint32_t last_x = 0;
        std::uint32_t first_y = 0;
        std::uint32_t last_y = 0;
        float left = 0.0f;
        float top = 0.0f;
        float right = 0.0f;
        float bottom = 0.0f;
    };
    [[nodiscard]] PixelSpan pixel_span(const rhi::Rect& rect) const;
    void blend(std::uint32_t x, std::uint32_t y, rhi::Color color);

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::uint8_t> pixels_;
};

} // namespace tessera::rendering::software
