// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstdint>
#include <span>
#include <vector>
namespace tessera::rendering::software {

// Minimal deterministic CPU canvas for golden references. It fills axis-aligned solid
// rectangles with hard pixel edges and, additively, composites 8-bit glyph coverage bitmaps for
// the CPU text golden. It is not a general vector rasterizer; the lavapipe path remains the
// full-fidelity software renderer.
class CpuCanvas {
public:
    CpuCanvas(std::uint32_t width, std::uint32_t height);

    void clear(rhi::Color color);
    void fill_rect(const rhi::Rect& rect, rhi::Color color);

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
    void blend(std::uint32_t x, std::uint32_t y, rhi::Color color);

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<std::uint8_t> pixels_;
};

} // namespace tessera::rendering::software
