// SPDX-License-Identifier: MIT
//
// The CPU half of the glass backdrop blur (docs/specs/glass-v1.md section 5): the separable
// Gaussian kernel (normalization and the linear-sampling pairing trick), the radius clamp, the
// half-resolution decision above `glassMaxBackdropArea`, and the paint dependency region of
// section 7.
//
// `glass_blur.frag` mirrors `pair_weight` and the tap loop exactly; these numbers are what the
// renderer records in its push constants and what the unit tests pin, so the shader and the engine
// can never disagree about the kernel.

#include "tessera/rendering/gpu/glass_blur.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::rendering::gpu {
namespace {

float sanitize(float value) {
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

std::uint32_t ceil_to_u32(float value) {
    if (!(value > 0.0f)) {
        return 0;
    }
    return static_cast<std::uint32_t>(std::ceil(value));
}

} // namespace

float clamp_glass_sigma(float sigma_css_px, const GlassLimits& limits) {
    const float max_radius = sanitize(limits.max_blur_radius);
    return std::clamp(sanitize(sigma_css_px), 0.0f, max_radius);
}

GlassKernel make_glass_kernel(float sigma_device_px) {
    GlassKernel kernel;
    kernel.sigma = sanitize(sigma_device_px);
    if (kernel.sigma <= 0.0f) {
        return kernel;
    }
    kernel.radius = ceil_to_u32(3.0f * kernel.sigma);
    // A linear tap covers two texels, so radius 0 (sigma < 1/3 texel) still needs one tap.
    kernel.taps = kernel.radius + 1;
    return kernel;
}

std::size_t glass_kernel_taps(const GlassKernel& kernel, std::span<GlassTap> out) {
    if (out.empty()) {
        return 0;
    }
    if (!kernel.active()) {
        out[0] = GlassTap{0.0f, 1.0f};
        return 1;
    }
    const int radius = static_cast<int>(kernel.radius);
    const float sigma = kernel.sigma;
    const float two_sigma_squared = 2.0f * sigma * sigma;
    const auto weight = [two_sigma_squared](float offset) {
        return std::exp(-(offset * offset) / two_sigma_squared);
    };

    float total = 0.0f;
    for (int offset = -radius; offset <= radius; ++offset) {
        total += weight(static_cast<float>(offset));
    }
    if (!(total > 0.0f)) {
        out[0] = GlassTap{0.0f, 1.0f};
        return 1;
    }

    // Pairs of adjacent texels `(a, a + 1)` from -radius upward, each replaced by one bilinear
    // sample at the pair's weighted center. The odd tap count leaves the far end unpaired.
    std::size_t count = 0;
    for (int a = -radius; a <= radius && count < kernel.taps && count < out.size(); a += 2) {
        const int b = std::min(a + 1, radius);
        const float wa = weight(static_cast<float>(a));
        const float wb = b == a ? 0.0f : weight(static_cast<float>(b));
        const float pair = wa + wb;
        if (pair <= 0.0f) {
            continue;
        }
        // An unpaired tap keeps its exact integer offset: `(a * wa) / wa` can round a float32
        // fraction above `radius` and break the reach invariant the kernel relies on.
        const float offset = b == a
                                 ? static_cast<float>(a)
                                 : (static_cast<float>(a) * wa + static_cast<float>(b) * wb) / pair;
        out[count] = GlassTap{offset, pair / total};
        ++count;
    }
    return count;
}

GlassBackdropPlan plan_glass_backdrop(rhi::Extent2D box_device, float sigma_css_px,
                                      float device_scale, const GlassLimits& limits) {
    GlassBackdropPlan plan;
    const float sigma = clamp_glass_sigma(sigma_css_px, limits);
    const float scale = std::isfinite(device_scale) && device_scale > 0.0f ? device_scale : 1.0f;
    const float sigma_device = sigma * scale;
    if (sigma_device <= 0.0f || box_device.width == 0 || box_device.height == 0) {
        return plan;
    }

    // Dependency region: the box grown by 3 sigma on every side (glass-v1 section 7).
    const auto reach = static_cast<std::uint32_t>(std::ceil(3.0f * sigma_device));
    plan.region_width = box_device.width + reach * 2;
    plan.region_height = box_device.height + reach * 2;
    const std::uint64_t area = static_cast<std::uint64_t>(plan.region_width) * plan.region_height;
    plan.half_resolution = area > limits.max_backdrop_area;

    if (plan.half_resolution) {
        // Odd extents round up so the blur target never loses a pixel of the region; the vertical
        // pass upsamples bilinearly.
        plan.blur_width = (plan.region_width + 1) / 2;
        plan.blur_height = (plan.region_height + 1) / 2;
        plan.kernel = make_glass_kernel(sigma_device * 0.5f);
    } else {
        plan.blur_width = plan.region_width;
        plan.blur_height = plan.region_height;
        plan.kernel = make_glass_kernel(sigma_device);
    }
    return plan;
}

} // namespace tessera::rendering::gpu
