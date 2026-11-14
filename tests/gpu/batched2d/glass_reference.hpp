// SPDX-License-Identifier: MIT
//
// CPU reference for the glass composite's pixels (test-only, not part of the engine build). It
// reproduces what `glass_blur.frag` does for a full-resolution blur target sized to the region:
// the separable Gaussian of `gpu::glass_kernel_taps` over the copied backdrop with the
// clamp-to-edge bilinear sampling the pooled sampled target uses, followed by the opacity-folded
// composite. The pixel tests derive their expectation from the region plan the renderer reports
// (`gpu::plan_glass_backdrop_region`), so the shader and this reference cannot drift.
//
// The backdrop it models is one colour left of a split and another from it on, constant along y:
// the vertical pass is then the identity, so a horizontal scan checks the kernel exactly.

#pragma once

#include "tessera/rendering/gpu/glass_blur.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace glass_reference {

namespace gpu = tessera::rendering::gpu;

struct Rgb {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
};

struct SplitBackdrop {
    Rgb left;
    Rgb right;
    float split_x = 0.0f;

    [[nodiscard]] Rgb texel(std::uint32_t frame_x) const {
        return static_cast<float>(frame_x) < split_x ? left : right;
    }
};

[[nodiscard]] inline Rgb mix(const Rgb& a, const Rgb& b, float t) {
    return Rgb{a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t};
}

// Bilinear sample of the copied backdrop region at a fractional region-local position, with the
// clamp-to-edge addressing the sampled target is created with.
[[nodiscard]] inline Rgb sample(const gpu::GlassBackdropRegion& region,
                                const SplitBackdrop& backdrop, float local_x) {
    const float max_local = static_cast<float>(region.source.width) - 1.0f;
    const float clamped = std::clamp(local_x, 0.0f, max_local);
    const auto low = static_cast<std::uint32_t>(std::floor(clamped));
    const std::uint32_t high = std::min(low + 1, region.source.width - 1);
    return mix(backdrop.texel(region.source.x + low), backdrop.texel(region.source.x + high),
               clamped - static_cast<float>(low));
}

// The horizontal Gaussian at frame pixel `frame_x`: destination pixel x samples at
// `x + tap.offset` region-local texels, which is `texel_step = 1 / blur width` in shader terms.
[[nodiscard]] inline Rgb blurred(const gpu::GlassBackdropRegion& region,
                                 const SplitBackdrop& backdrop, std::uint32_t frame_x) {
    const float local = static_cast<float>(frame_x) - static_cast<float>(region.source.x);
    std::array<gpu::GlassTap, 64> taps{};
    const std::size_t tap_count =
        gpu::glass_kernel_taps(region.kernel, std::span<gpu::GlassTap>(taps));
    Rgb sum;
    for (std::size_t tap = 0; tap < tap_count; ++tap) {
        const Rgb value = sample(region, backdrop, local + taps[tap].offset);
        sum.r += value.r * taps[tap].weight;
        sum.g += value.g * taps[tap].weight;
        sum.b += value.b * taps[tap].weight;
    }
    return sum;
}

// What the composite pass writes over an opaque backdrop with straight-alpha blending:
// `opacity * blurred + (1 - opacity) * original` (glass-v1 section 3, `BackdropDraw::opacity`).
[[nodiscard]] inline Rgb composite(const Rgb& blurred_color, const Rgb& original, float opacity) {
    return Rgb{opacity * blurred_color.r + (1.0f - opacity) * original.r,
               opacity * blurred_color.g + (1.0f - opacity) * original.g,
               opacity * blurred_color.b + (1.0f - opacity) * original.b};
}

} // namespace glass_reference
