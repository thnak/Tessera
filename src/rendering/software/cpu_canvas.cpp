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

std::uint32_t to_channel(float value) {
    return static_cast<std::uint32_t>(to_byte(value));
}

std::uint8_t blend_channel(std::uint32_t source, std::uint32_t destination, std::uint32_t alpha) {
    return static_cast<std::uint8_t>((source * alpha + destination * (255 - alpha) + 127) / 255);
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

void CpuCanvas::blend_coverage(std::int32_t x, std::int32_t y, std::uint32_t width,
                               std::uint32_t height, std::span<const std::uint8_t> coverage,
                               rhi::Color color, const rhi::Rect* clip) {
    const std::size_t pixel_count = static_cast<std::size_t>(width) * height;
    if (width == 0 || height == 0 || coverage.size() < pixel_count || color.a <= 0.0f) {
        return;
    }

    const std::uint32_t source_r = to_channel(color.r);
    const std::uint32_t source_g = to_channel(color.g);
    const std::uint32_t source_b = to_channel(color.b);
    const std::uint32_t source_alpha = to_channel(color.a);
    if (source_alpha == 0) {
        return;
    }

    // Pixel-center clipping matches fill_rect: a destination pixel is kept when its center lies in
    // [clip.x, clip.x + clip.width) x [clip.y, clip.y + clip.height).
    std::int64_t clip_x0 = 0;
    std::int64_t clip_y0 = 0;
    std::int64_t clip_x1 = static_cast<std::int64_t>(width_);
    std::int64_t clip_y1 = static_cast<std::int64_t>(height_);
    if (clip != nullptr && clip->width > 0.0f && clip->height > 0.0f) {
        clip_x0 = static_cast<std::int64_t>(std::ceil(clip->x - 0.5f));
        clip_y0 = static_cast<std::int64_t>(std::ceil(clip->y - 0.5f));
        clip_x1 = static_cast<std::int64_t>(std::ceil(clip->x + clip->width - 0.5f));
        clip_y1 = static_cast<std::int64_t>(std::ceil(clip->y + clip->height - 0.5f));
        clip_x0 = std::max<std::int64_t>(clip_x0, 0);
        clip_y0 = std::max<std::int64_t>(clip_y0, 0);
        clip_x1 = std::min<std::int64_t>(clip_x1, static_cast<std::int64_t>(width_));
        clip_y1 = std::min<std::int64_t>(clip_y1, static_cast<std::int64_t>(height_));
    }
    if (clip_x0 >= clip_x1 || clip_y0 >= clip_y1) {
        return;
    }

    for (std::int64_t py = std::max<std::int64_t>(y, clip_y0);
         py < std::min<std::int64_t>(static_cast<std::int64_t>(y) + height, clip_y1); ++py) {
        const auto source_row = static_cast<std::uint32_t>(py - y);
        for (std::int64_t px = std::max<std::int64_t>(x, clip_x0);
             px < std::min<std::int64_t>(static_cast<std::int64_t>(x) + width, clip_x1); ++px) {
            const auto source_column = static_cast<std::uint32_t>(px - x);
            const std::uint32_t font_coverage =
                coverage[static_cast<std::size_t>(source_row) * width + source_column];
            const std::uint32_t pixel_alpha = (source_alpha * font_coverage + 127) / 255;
            if (pixel_alpha == 0) {
                continue;
            }
            const std::size_t index =
                (static_cast<std::size_t>(py) * width_ + static_cast<std::size_t>(px)) * 4;
            if (pixel_alpha == 255) {
                pixels_[index] = static_cast<std::uint8_t>(source_r);
                pixels_[index + 1] = static_cast<std::uint8_t>(source_g);
                pixels_[index + 2] = static_cast<std::uint8_t>(source_b);
            } else {
                pixels_[index] = blend_channel(source_r, pixels_[index], pixel_alpha);
                pixels_[index + 1] = blend_channel(source_g, pixels_[index + 1], pixel_alpha);
                pixels_[index + 2] = blend_channel(source_b, pixels_[index + 2], pixel_alpha);
            }
            // The canvas is opaque; glyph alpha only modulates the composite above.
            pixels_[index + 3] = 255;
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
