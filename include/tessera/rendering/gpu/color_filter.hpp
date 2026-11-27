// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/layer_filter.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::rendering::gpu {

// The reference for the CSS colour filter functions (Filter Effects Module Level 1, section
// "Filter Functions"): what `glass_blur.frag` mode 3 and the CPU twin both compute, and what the
// unit tests pin against Chromium measurements (tests/unit/color_filter_tests.cpp).
//
// A function works on the straight-alpha colour: the layer's premultiplied pixel is divided by its
// alpha first and multiplied again after, so a transparent pixel stays transparent whatever the
// matrix adds (invert, contrast), and `opacity()` scales the alpha only. Every function clamps its
// result to [0, 1] before the next one runs; that is observable (`brightness(2) contrast(0.5)` is
// not one folded matrix) and Chromium does it, so a chain cannot be folded into a single matrix.
struct StraightPixel {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 0.0f;
};

namespace color_filter_detail {

inline float saturate01(float value) {
    return std::clamp(value, 0.0f, 1.0f);
}

// Rec. 709 luma weights of the grayscale() matrix, and the rounded ones the spec gives saturate()
// and hue-rotate(). The spec keeps both sets, so both are reproduced.
inline constexpr float kLumaR = 0.2126f;
inline constexpr float kLumaG = 0.7152f;
inline constexpr float kLumaB = 0.0722f;

} // namespace color_filter_detail

// Applies one function to `pixel`. `amount` is the computed value (see ColorFunction).
inline void apply_color_function(ColorFunction function, float amount, StraightPixel& pixel) {
    using color_filter_detail::saturate01;
    const float r = pixel.r;
    const float g = pixel.g;
    const float b = pixel.b;
    switch (function) {
    case ColorFunction::Brightness:
        pixel.r = r * amount;
        pixel.g = g * amount;
        pixel.b = b * amount;
        break;
    case ColorFunction::Contrast: {
        const float offset = (1.0f - amount) * 0.5f;
        pixel.r = r * amount + offset;
        pixel.g = g * amount + offset;
        pixel.b = b * amount + offset;
        break;
    }
    case ColorFunction::Invert: {
        const float k = saturate01(amount);
        pixel.r = r * (1.0f - 2.0f * k) + k;
        pixel.g = g * (1.0f - 2.0f * k) + k;
        pixel.b = b * (1.0f - 2.0f * k) + k;
        break;
    }
    case ColorFunction::Opacity:
        pixel.a *= saturate01(amount);
        break;
    case ColorFunction::Grayscale: {
        const float k = 1.0f - saturate01(amount);
        using color_filter_detail::kLumaB;
        using color_filter_detail::kLumaG;
        using color_filter_detail::kLumaR;
        pixel.r =
            (kLumaR + 0.7874f * k) * r + (kLumaG - kLumaG * k) * g + (kLumaB - kLumaB * k) * b;
        pixel.g =
            (kLumaR - kLumaR * k) * r + (kLumaG + 0.2848f * k) * g + (kLumaB - kLumaB * k) * b;
        pixel.b =
            (kLumaR - kLumaR * k) * r + (kLumaG - kLumaG * k) * g + (kLumaB + 0.9278f * k) * b;
        break;
    }
    case ColorFunction::Sepia: {
        const float k = 1.0f - saturate01(amount);
        pixel.r = (0.393f + 0.607f * k) * r + (0.769f - 0.769f * k) * g + (0.189f - 0.189f * k) * b;
        pixel.g = (0.349f - 0.349f * k) * r + (0.686f + 0.314f * k) * g + (0.168f - 0.168f * k) * b;
        pixel.b = (0.272f - 0.272f * k) * r + (0.534f - 0.534f * k) * g + (0.131f + 0.869f * k) * b;
        break;
    }
    case ColorFunction::Saturate: {
        const float s = amount;
        pixel.r = (0.213f + 0.787f * s) * r + (0.715f - 0.715f * s) * g + (0.072f - 0.072f * s) * b;
        pixel.g = (0.213f - 0.213f * s) * r + (0.715f + 0.285f * s) * g + (0.072f - 0.072f * s) * b;
        pixel.b = (0.213f - 0.213f * s) * r + (0.715f - 0.715f * s) * g + (0.072f + 0.928f * s) * b;
        break;
    }
    case ColorFunction::HueRotate: {
        const float c = std::cos(amount);
        const float s = std::sin(amount);
        pixel.r = (0.213f + c * 0.787f - s * 0.213f) * r + (0.715f - c * 0.715f - s * 0.715f) * g +
                  (0.072f - c * 0.072f + s * 0.928f) * b;
        pixel.g = (0.213f - c * 0.213f + s * 0.143f) * r + (0.715f + c * 0.285f + s * 0.140f) * g +
                  (0.072f - c * 0.072f - s * 0.283f) * b;
        pixel.b = (0.213f - c * 0.213f - s * 0.787f) * r + (0.715f - c * 0.715f + s * 0.715f) * g +
                  (0.072f + c * 0.928f + s * 0.072f) * b;
        break;
    }
    case ColorFunction::None:
        return;
    }
    pixel.r = saturate01(pixel.r);
    pixel.g = saturate01(pixel.g);
    pixel.b = saturate01(pixel.b);
    pixel.a = saturate01(pixel.a);
}

// Applies every function of `op` (a Kind::ColorFunctions op) in order.
inline void apply_color_functions(const LayerFilterOp& op, StraightPixel& pixel) {
    for (std::size_t index = 0; index < kColorFunctionsPerOp; ++index) {
        const ColorFunction function = op.color_function(index);
        if (function == ColorFunction::None) {
            break;
        }
        apply_color_function(function, op.color_amount(index), pixel);
    }
}

} // namespace tessera::rendering::gpu
