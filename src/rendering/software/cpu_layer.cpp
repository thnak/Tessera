// SPDX-License-Identifier: MIT

#include "tessera/rendering/gpu/color_filter.hpp"
#include "tessera/rendering/software/cpu_layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace tessera::rendering::software {
namespace {

bool same_size(const CpuCanvas& a, const CpuCanvas& b) {
    return a.width() == b.width() && a.height() == b.height();
}

// Alpha in 0..255 of the pixel at byte offset `index`: 255 minus the rounded mean of the three
// channel differences between the white-backed and the black-backed canvas.
int layer_alpha(const std::uint8_t* black, const std::uint8_t* white) {
    const int spread = (white[0] - black[0]) + (white[1] - black[1]) + (white[2] - black[2]);
    return std::clamp(255 - (spread + (spread >= 0 ? 1 : -1)) / 3, 0, 255);
}

std::uint8_t to_byte(float value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0f, 255.0f)));
}

} // namespace

void composite_layer(CpuCanvas& destination, const CpuCanvas& black, const CpuCanvas& white,
                     const rhi::Rect& region, float opacity) {
    if (!same_size(destination, black) || !same_size(destination, white) || region.width <= 0.0f ||
        region.height <= 0.0f || opacity <= 0.0f) {
        return;
    }
    opacity = std::min(opacity, 1.0f);
    const std::uint32_t width = destination.width();
    const std::uint32_t height = destination.height();
    const auto first_x = static_cast<std::int64_t>(std::max(0.0f, std::ceil(region.x - 0.5f)));
    const auto first_y = static_cast<std::int64_t>(std::max(0.0f, std::ceil(region.y - 0.5f)));
    const auto last_x = std::min<std::int64_t>(
        width, static_cast<std::int64_t>(std::ceil(region.x + region.width - 0.5f)));
    const auto last_y = std::min<std::int64_t>(
        height, static_cast<std::int64_t>(std::ceil(region.y + region.height - 0.5f)));
    for (std::int64_t y = first_y; y < last_y; ++y) {
        for (std::int64_t x = first_x; x < last_x; ++x) {
            const std::size_t index = (static_cast<std::size_t>(y) * width + x) * 4;
            const int alpha = layer_alpha(&black.pixels()[index], &white.pixels()[index]);
            const float keep = 1.0f - opacity * (static_cast<float>(alpha) / 255.0f);
            std::uint8_t* out = &destination.pixels()[index];
            for (std::size_t channel = 0; channel < 3; ++channel) {
                out[channel] =
                    to_byte(static_cast<float>(black.pixels()[index + channel]) * opacity +
                            static_cast<float>(out[channel]) * keep);
            }
            out[3] = 255;
        }
    }
}

void apply_layer_mask(CpuCanvas& black, CpuCanvas& white, const CpuCanvas& mask_black,
                      const CpuCanvas& mask_white) {
    if (!same_size(black, white) || !same_size(black, mask_black) ||
        !same_size(black, mask_white)) {
        return;
    }
    const std::size_t pixel_count = static_cast<std::size_t>(black.width()) * black.height();
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
        const std::size_t index = pixel * 4;
        const float mask = static_cast<float>(layer_alpha(&mask_black.pixels()[index],
                                                          &mask_white.pixels()[index])) /
                           255.0f;
        const float alpha =
            static_cast<float>(layer_alpha(&black.pixels()[index], &white.pixels()[index]));
        const float masked_alpha = std::round(alpha * mask);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float premultiplied =
                std::round(static_cast<float>(black.pixels()[index + channel]) * mask);
            black.pixels()[index + channel] = to_byte(premultiplied);
            white.pixels()[index + channel] = to_byte(premultiplied + (255.0f - masked_alpha));
        }
    }
}

void combine_mask_part(CpuCanvas& mask_black, CpuCanvas& mask_white, const CpuCanvas& part_black,
                       const CpuCanvas& part_white, gpu::MaskCompositeOp op) {
    if (!same_size(mask_black, mask_white) || !same_size(mask_black, part_black) ||
        !same_size(mask_black, part_white)) {
        return;
    }
    const std::size_t pixel_count =
        static_cast<std::size_t>(mask_black.width()) * mask_black.height();
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
        const std::size_t index = pixel * 4;
        const float below = static_cast<float>(layer_alpha(&mask_black.pixels()[index],
                                                           &mask_white.pixels()[index])) /
                            255.0f;
        const float layer = static_cast<float>(layer_alpha(&part_black.pixels()[index],
                                                           &part_white.pixels()[index])) /
                            255.0f;
        float result = 0.0f;
        switch (op) {
        case gpu::MaskCompositeOp::Subtract:
            result = layer * (1.0f - below);
            break;
        case gpu::MaskCompositeOp::Intersect:
            result = layer * below;
            break;
        case gpu::MaskCompositeOp::Exclude:
            result = layer * (1.0f - below) + below * (1.0f - layer);
            break;
        case gpu::MaskCompositeOp::Add:
        default:
            result = layer + below * (1.0f - layer);
            break;
        }
        const float alpha = std::round(result * 255.0f);
        for (std::size_t channel = 0; channel < 3; ++channel) {
            mask_black.pixels()[index + channel] = 0;
            mask_white.pixels()[index + channel] = to_byte(255.0f - alpha);
        }
    }
}

void apply_layer_color_filter(CpuCanvas& black, CpuCanvas& white, const gpu::LayerFilterOp& op) {
    if (!same_size(black, white) || op.kind != gpu::LayerFilterOp::Kind::ColorFunctions ||
        op.color_count() == 0) {
        return;
    }
    const std::size_t pixel_count = static_cast<std::size_t>(black.width()) * black.height();
    for (std::size_t pixel = 0; pixel < pixel_count; ++pixel) {
        std::uint8_t* const dark = &black.pixels()[pixel * 4];
        std::uint8_t* const light = &white.pixels()[pixel * 4];
        const int alpha_byte = layer_alpha(dark, light);
        if (alpha_byte == 0) {
            continue; // nothing drawn: every function keeps a transparent pixel transparent
        }
        const float alpha = static_cast<float>(alpha_byte) / 255.0f;
        gpu::StraightPixel straight;
        straight.r = std::min(static_cast<float>(dark[0]) / 255.0f / alpha, 1.0f);
        straight.g = std::min(static_cast<float>(dark[1]) / 255.0f / alpha, 1.0f);
        straight.b = std::min(static_cast<float>(dark[2]) / 255.0f / alpha, 1.0f);
        straight.a = alpha;
        gpu::apply_color_functions(op, straight);
        const float new_alpha = std::round(straight.a * 255.0f);
        const float colour[3] = {straight.r, straight.g, straight.b};
        for (std::size_t channel = 0; channel < 3; ++channel) {
            const float premultiplied = std::round(colour[channel] * straight.a * 255.0f);
            dark[channel] = to_byte(premultiplied);
            light[channel] = to_byte(premultiplied + (255.0f - new_alpha));
        }
    }
}

} // namespace tessera::rendering::software
