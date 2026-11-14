// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/clip.hpp"
#include "tessera/rendering/rhi/device.hpp"
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

// The offscreen region one glass node needs, in frame-target pixels: the dependency box clamped to
// the target, the blur-target extent (half the region above `max_backdrop_area`) and the kernel.
// This is what `Batched2DRenderer::draw_backdrop` copies and blurs; the renderer and the pixel
// tests share it, so an expectation can be derived instead of guessed.
struct GlassBackdropRegion {
    bool active = false;
    rhi::Scissor source; // region copied out of the frame target (clamped, integer pixels)
    rhi::Extent2D blur;  // requested blur-target extent (half the region at half resolution)
    bool half_resolution = false;
    GlassKernel kernel;
};

// `box` is the node's border box in paint coordinates (device px before the renderer's content
// scale); `target_extent` is the frame target the box is painted into. Inactive for an empty box,
// sigma <= 0 or a region entirely outside the target.
[[nodiscard]] GlassBackdropRegion plan_glass_backdrop_region(const rhi::Rect& box,
                                                             float sigma_css_px, float device_scale,
                                                             const GlassLimits& limits,
                                                             rhi::Extent2D target_extent);

// One resolved glass node the batched renderer can execute (issue #244, glass-v1 sections 3-5), in
// paint coordinates: `clip` is the node's border box shaped by its border radius (the composite
// clip), the filter values are the resolved `backdrop-filter` operands, and `device_scale` converts
// the CSS-px sigma to device pixels. The renderer applies its own content scale on top.
struct GlassBackdropDraw {
    clip::RoundedRect clip;
    float sigma = 0.0f; // CSS px, already clamped to glassMaxBlurRadius by the paint pass
    float opacity = 1.0f;
    float saturate = 1.0f;
    float brightness = 1.0f;
    float contrast = 1.0f;
    float device_scale = 1.0f;
};

// The two pipelines one glass composite records. `blur` writes a pooled target and must not blend
// (the target holds the straight-alpha blur the composite pass reads); `composite` draws into the
// frame target with alpha blending, so the rounded clip antialiases and `opacity` folds into the
// result. Both are borrowed for the renderer's lifetime, like the 2D pipeline.
struct GlassPipelines {
    rhi::Pipeline* blur = nullptr;
    rhi::Pipeline* composite = nullptr;
};

// The push-constant block `glass_blur.frag` reads, one block per pass. It mirrors the shader's
// standard Vulkan layout (tools/shaders/manifest.json declares the same 112 bytes and field
// offsets; the shader-parity test compares the manifest against `Batched2DPushConstants` only, so
// the layout is pinned here). `mode` picks the pass: 0 = horizontal blur into a blur target, 1 =
// vertical blur into the frame target plus the color matrix and the rounded composite clip, with
// `opacity` folded into the composite alpha.
//
// A sampled render target is read through `bind_sampled_target` with the target's sampler state
// (linear + clamp to edge), which is what the tap loop assumes.
struct GlassPushConstants {
    float texel_step_x = 0.0f; // source-space uv step per destination pixel (see draw_backdrop)
    float texel_step_y = 0.0f;
    float direction_x = 1.0f; // (1, 0) horizontal, (0, 1) vertical
    float direction_y = 0.0f;
    float sigma = 0.0f;           // Gaussian sigma in blur-target texels
    float radius = 0.0f;          // ceil(3 * sigma), the CPU kernel's integer radius
    float region_origin_x = 0.0f; // destination-space origin of the blurred region
    float region_origin_y = 0.0f;
    float region_size_x = 0.0f; // destination-space size of the blurred region
    float region_size_y = 0.0f;
    // The standard layout aligns `box` (a vec4) to 16 bytes, so two floats are padding.
    float padding_0 = 0.0f;
    float padding_1 = 0.0f;
    float box_x = 0.0f; // xy = composite box origin, zw = box size (destination space)
    float box_y = 0.0f;
    float box_width = 0.0f;
    float box_height = 0.0f;
    float radius_tl = 0.0f; // top-left, top-right, bottom-right, bottom-left corner radii
    float radius_tr = 0.0f;
    float radius_br = 0.0f;
    float radius_bl = 0.0f;
    float saturate = 1.0f; // saturate, brightness, contrast (vertical pass only)
    float brightness = 1.0f;
    float contrast = 1.0f;
    float mode = 0.0f;    // 0 = horizontal into the blur target, 1 = vertical + composite
    float opacity = 1.0f; // composite alpha weight (vertical pass only)
    // std140 pads the block to a 16-byte multiple; the shader never reads these.
    float padding_2 = 0.0f;
    float padding_3 = 0.0f;
    float padding_4 = 0.0f;
};

static_assert(sizeof(GlassPushConstants) == 112,
              "glass_blur push constants must match the shader's 112-byte block");
static_assert(offsetof(GlassPushConstants, box_x) == 48, "glass_blur vec4 box is 16-byte aligned");
static_assert(offsetof(GlassPushConstants, saturate) == 80, "glass_blur factors follow the radii");
static_assert(offsetof(GlassPushConstants, mode) == 92, "glass_blur mode precedes the opacity");
static_assert(offsetof(GlassPushConstants, opacity) == 96, "glass_blur opacity is the last field");

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
