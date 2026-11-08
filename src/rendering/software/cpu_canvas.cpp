// SPDX-License-Identifier: MIT

#include "tessera/rendering/software/cpu_canvas.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::rendering::software {
namespace {

std::uint8_t to_byte(float value) {
    const float clamped = std::clamp(value, 0.0f, 1.0f);
    return static_cast<std::uint8_t>(std::lround(clamped * 255.0f));
}

} // namespace

CpuCanvas::CpuCanvas(std::uint32_t width, std::uint32_t height)
    : width_(width),
      height_(height),
      pixels_(static_cast<std::size_t>(width) * height * 4, 0) {}

void CpuCanvas::clear(rhi::Color color) {
    fill_rect({0, 0, static_cast<float>(width_), static_cast<float>(height_)}, color);
}

void CpuCanvas::fill_rect(const rhi::Rect& rect, rhi::Color color) {
    if (rect.width <= 0.0f || rect.height <= 0.0f || color.a <= 0.0f) {
        return;
    }
    const float left = std::max(0.0f, rect.x);
    const float top = std::max(0.0f, rect.y);
    const float right = std::min(static_cast<float>(width_), rect.x + rect.width);
    const float bottom = std::min(static_cast<float>(height_), rect.y + rect.height);
    if (left >= right || top >= bottom) {
        return;
    }

    const auto first_x = static_cast<std::uint32_t>(std::floor(left));
    const auto first_y = static_cast<std::uint32_t>(std::floor(top));
    const auto last_x = static_cast<std::uint32_t>(std::ceil(right));
    const auto last_y = static_cast<std::uint32_t>(std::ceil(bottom));
    for (std::uint32_t y = first_y; y < last_y && y < height_; ++y) {
        const float center_y = static_cast<float>(y) + 0.5f;
        if (center_y < top || center_y >= bottom) {
            continue;
        }
        for (std::uint32_t x = first_x; x < last_x && x < width_; ++x) {
            const float center_x = static_cast<float>(x) + 0.5f;
            if (center_x < left || center_x >= right) {
                continue;
            }
            blend(x, y, color);
        }
    }
}

void CpuCanvas::blend(std::uint32_t x, std::uint32_t y, rhi::Color color) {
    const std::size_t index = (static_cast<std::size_t>(y) * width_ + x) * 4;
    const float alpha = color.a;
    const float inverse = 1.0f - alpha;
    pixels_[index] = to_byte(color.r * alpha + (pixels_[index] / 255.0f) * inverse);
    pixels_[index + 1] = to_byte(color.g * alpha + (pixels_[index + 1] / 255.0f) * inverse);
    pixels_[index + 2] = to_byte(color.b * alpha + (pixels_[index + 2] / 255.0f) * inverse);
    pixels_[index + 3] = 255;
}

} // namespace tessera::rendering::software
