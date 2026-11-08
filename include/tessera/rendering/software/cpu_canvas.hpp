// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cstdint>
#include <span>
#include <vector>

namespace tessera::rendering::software {

// Minimal deterministic CPU canvas for golden references. It fills axis-aligned solid
// rectangles with hard pixel edges (no AA, paths, text, or transforms); the lavapipe path
// remains the full-fidelity software renderer. Using it this way does not bypass the
// "no path rasterizer" rule because it is not a general vector rasterizer.
class CpuCanvas {
public:
    CpuCanvas(std::uint32_t width, std::uint32_t height);

    void clear(rhi::Color color);
    void fill_rect(const rhi::Rect& rect, rhi::Color color);

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
