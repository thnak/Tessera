// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/types.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tessera::rendering::gpu {

// Cost limits for `backdrop-filter` glass (docs/specs/glass-v1.md sections 5 and 8). They bound
// the cost and never turn the effect off: the runtime config keys are `glassMaxBlurRadius`,
// `glassMaxBlurTargets` (the offscreen pool) and `glassMaxBackdropArea`.
struct GlassLimits {
    float max_blur_radius = 64.0f;               // CSS px
    std::uint64_t max_backdrop_area = 4'000'000; // device pixels; 4 Mpx
};

// Clamps a declared blur sigma to `[0, limits.max_blur_radius]`. Non-finite or negative values
// become 0 (a pass-through); the declaration is never dropped for an out-of-range radius.
[[nodiscard]] float clamp_glass_sigma(float sigma_css_px, const GlassLimits& limits);

// The separable-Gaussian kernel of one blur pass (glass-v1 section 5): sigma in blur-target texels,
// the integer reach `ceil(3 * sigma)` and the linear-sampling tap count. Each tap combines two
// adjacent texels through a linear filter, so the count is `radius + 1` instead of `2 * radius +
// 1`.
struct GlassKernel {
    float sigma = 0.0f;       // blur-target texels
    std::uint32_t radius = 0; // ceil(3 * sigma)
    std::uint32_t taps = 0;   // radius + 1

    [[nodiscard]] bool active() const {
        return sigma > 0.0f;
    }
};

[[nodiscard]] GlassKernel make_glass_kernel(float sigma_device_px);

// One linear-sampling tap: a fractional texel offset and the normalized weight of the texel pair.
struct GlassTap {
    float offset = 0.0f;
    float weight = 0.0f;
};

// Fills `out` with the kernel's normalized taps and returns how many were written. The weights sum
// to 1 for any sigma > 0, and a single tap at offset 0 when sigma == 0. `glass_blur.frag` computes
// the same `pair_weight` formula in the same order, so this is the CPU reference the unit tests and
// the renderer's tap-count decision share.
[[nodiscard]] std::size_t glass_kernel_taps(const GlassKernel& kernel, std::span<GlassTap> out);

// The plan for one glass node's backdrop: the dependency region (box grown by 3 sigma) and the
// blur-target extents. Above `limits.max_backdrop_area` the blur runs at half resolution and the
// vertical pass upsamples bilinearly (a quality choice, not a removal of the effect).
struct GlassBackdropPlan {
    std::uint32_t region_width = 0; // full-resolution dependency region, device px
    std::uint32_t region_height = 0;
    std::uint32_t blur_width = 0; // blur-target extents (half of the region above the area limit)
    std::uint32_t blur_height = 0;
    bool half_resolution = false;
    GlassKernel kernel;
};

// `box` is the node's border box in device pixels. Returns a pass-through plan (inactive kernel,
// zero extents) for an empty box or sigma <= 0.
[[nodiscard]] GlassBackdropPlan plan_glass_backdrop(rhi::Extent2D box_device, float sigma_css_px,
                                                    float device_scale, const GlassLimits& limits);

// The paint dependency region of a glass node: its box grown by `3 * sigma` on every side, in
// device pixels (glass-v1 section 7). Damage intersecting this region must repaint the glass node.
// Inline and header-only so the UI damage layer can use the one definition without linking the
// renderer: this is a formula, not a GPU resource.
[[nodiscard]] inline rhi::Rect glass_dependency_rect(const rhi::Rect& box, float sigma_css_px,
                                                     float device_scale) {
    const float scale = std::isfinite(device_scale) && device_scale > 0.0f ? device_scale : 1.0f;
    const float sigma = std::isfinite(sigma_css_px) && sigma_css_px > 0.0f ? sigma_css_px : 0.0f;
    const float reach = 3.0f * sigma * scale;
    return rhi::Rect{box.x - reach, box.y - reach, box.width + reach * 2.0f,
                     box.height + reach * 2.0f};
}

} // namespace tessera::rendering::gpu
