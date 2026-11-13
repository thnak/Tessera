// SPDX-License-Identifier: MIT
//
// CPU half of the glass blur (issue #244, docs/specs/glass-v1.md sections 5 and 7): the separable
// Gaussian kernel (reach, linear-sampling tap count, normalization), the radius clamp, the
// half-resolution decision above `glassMaxBackdropArea`, the paint dependency region, and a
// two-pass reference blur over a synthetic image that pins the shader's math.

#include "tessera/rendering/gpu/glass_blur.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <vector>

namespace {

using tessera::rendering::gpu::clamp_glass_sigma;
using tessera::rendering::gpu::glass_dependency_rect;
using tessera::rendering::gpu::glass_kernel_taps;
using tessera::rendering::gpu::GlassBackdropPlan;
using tessera::rendering::gpu::GlassKernel;
using tessera::rendering::gpu::GlassLimits;
using tessera::rendering::gpu::GlassTap;
using tessera::rendering::gpu::make_glass_kernel;
using tessera::rendering::gpu::plan_glass_backdrop;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "glass_blur: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected, float tolerance) {
    return std::abs(value - expected) <= tolerance;
}

bool clamp_is(float declared, const GlassLimits& limits, float expected) {
    return near(clamp_glass_sigma(declared, limits), expected, 1.0e-6f);
}

bool kernel_shape() {
    bool ok = expect(make_glass_kernel(0.0f).taps == 0, "sigma 0 has no kernel");
    // Reach is ceil(3 * sigma); a linear tap combines two texels, so taps = radius + 1.
    const float sigmas[] = {0.5f, 1.0f, 2.5f, 8.0f, 20.0f, 64.0f};
    for (const float sigma : sigmas) {
        const GlassKernel kernel = make_glass_kernel(sigma);
        const auto radius = static_cast<std::uint32_t>(std::ceil(3.0f * sigma));
        ok = expect(kernel.sigma == sigma && kernel.radius == radius && kernel.taps == radius + 1 &&
                        kernel.active(),
                    "radius = ceil(3 sigma), taps = radius + 1") &&
             ok;
    }
    return ok;
}

bool kernel_normalized() {
    bool ok = true;
    const float sigmas[] = {0.25f, 1.0f, 3.0f, 12.0f, 64.0f};
    std::vector<GlassTap> taps;
    float previous_offset = 0.0f;
    for (const float sigma : sigmas) {
        const GlassKernel kernel = make_glass_kernel(sigma);
        taps.resize(kernel.taps);
        const std::size_t count = glass_kernel_taps(kernel, std::span<GlassTap>(taps));
        float total = 0.0f;
        float absolute_offset = 0.0f;
        bool offsets_in_reach = true;
        for (std::size_t index = 0; index < count; ++index) {
            total += taps[index].weight;
            absolute_offset += std::abs(taps[index].offset) * taps[index].weight;
            offsets_in_reach = offsets_in_reach &&
                               std::abs(taps[index].offset) <= static_cast<float>(kernel.radius);
        }
        ok = expect(count == kernel.taps, "tap count equals radius + 1") && ok;
        ok = expect(near(total, 1.0f, 1.0e-5f), "kernel weights are normalized") && ok;
        ok = expect(offsets_in_reach, "every tap offset is inside the reach") && ok;
        // A wider sigma spreads the same weight further out.
        ok = expect(absolute_offset > previous_offset, "mean tap distance grows with sigma") && ok;
        previous_offset = absolute_offset;
    }
    // A pass-through sigma yields one tap at the center with all the weight.
    const GlassKernel passthrough = make_glass_kernel(0.0f);
    taps.resize(1);
    const std::size_t count = glass_kernel_taps(passthrough, std::span<GlassTap>(taps));
    ok = expect(count == 1 && taps[0].offset == 0.0f && taps[0].weight == 1.0f,
                "sigma 0 is one centered tap") &&
         ok;
    return ok;
}

// --- two-pass reference blur over a small image -------------------------------------------------
//
// Mirrors `glass_blur.frag`: one kernel per direction, clamp to edge, linear-sampling taps at their
// fractional offsets, normalized by the summed weights.

using Image = std::vector<float>;

Image blur_pass(const Image& source, std::uint32_t width, std::uint32_t height,
                const GlassKernel& kernel, bool horizontal) {
    std::vector<GlassTap> taps(kernel.taps);
    const std::size_t count = glass_kernel_taps(kernel, std::span<GlassTap>(taps));
    Image result(source.size(), 0.0f);
    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            float sum = 0.0f;
            for (std::size_t tap = 0; tap < count; ++tap) {
                const float center_x = horizontal ? static_cast<float>(x) + 0.5f + taps[tap].offset
                                                  : static_cast<float>(x) + 0.5f;
                const float center_y = horizontal ? static_cast<float>(y) + 0.5f
                                                  : static_cast<float>(y) + 0.5f + taps[tap].offset;
                // Linear sample at the pixel-center positions, clamped to the edge.
                const auto fetch = [&](float px, float py) {
                    const float cx = px - 0.5f;
                    const float cy = py - 0.5f;
                    const auto x0 = static_cast<std::int32_t>(std::floor(cx));
                    const auto y0 = static_cast<std::int32_t>(std::floor(cy));
                    const float tx = cx - static_cast<float>(x0);
                    const float ty = cy - static_cast<float>(y0);
                    const auto clamp_x = [width](std::int32_t value) {
                        return static_cast<std::size_t>(
                            std::clamp(value, 0, static_cast<std::int32_t>(width) - 1));
                    };
                    const auto clamp_y = [height](std::int32_t value) {
                        return static_cast<std::size_t>(
                            std::clamp(value, 0, static_cast<std::int32_t>(height) - 1));
                    };
                    const float v00 = source[clamp_y(y0) * width + clamp_x(x0)];
                    const float v10 = source[clamp_y(y0) * width + clamp_x(x0 + 1)];
                    const float v01 = source[clamp_y(y0 + 1) * width + clamp_x(x0)];
                    const float v11 = source[clamp_y(y0 + 1) * width + clamp_x(x0 + 1)];
                    return (v00 * (1.0f - tx) + v10 * tx) * (1.0f - ty) +
                           (v01 * (1.0f - tx) + v11 * tx) * ty;
                };
                sum += fetch(center_x, center_y) * taps[tap].weight;
            }
            result[static_cast<std::size_t>(y) * width + x] = sum;
        }
    }
    return result;
}

Image blur(const Image& source, std::uint32_t width, std::uint32_t height, float sigma) {
    const GlassKernel kernel = make_glass_kernel(sigma);
    if (!kernel.active()) {
        return source;
    }
    const Image horizontal = blur_pass(source, width, height, kernel, true);
    return blur_pass(horizontal, width, height, kernel, false);
}

bool constant_input_is_unchanged() {
    // Normalization + clamp-to-edge: a flat backdrop stays flat (a glass bar at a window edge must
    // not darken).
    constexpr std::uint32_t kWidth = 12;
    constexpr std::uint32_t kHeight = 8;
    Image flat(kWidth * kHeight, 0.6f);
    const Image blurred = blur(flat, kWidth, kHeight, 9.0f);
    bool ok = true;
    for (const float value : blurred) {
        ok = expect(near(value, 0.6f, 1.0e-5f), "a constant backdrop is unchanged") && ok;
        if (!ok) {
            break;
        }
    }
    return ok;
}

bool impulse_spreads_and_conserves() {
    constexpr std::uint32_t kSize = 33;
    Image impulse(kSize * kSize, 0.0f);
    impulse[16 * kSize + 16] = 1.0f;
    const Image blurred = blur(impulse, kSize, kSize, 2.0f);

    double total = 0.0;
    for (const float value : blurred) {
        total += value;
    }
    bool ok = expect(std::abs(total - 1.0) < 1.0e-4, "the blur preserves total energy");
    ok = expect(blurred[16 * kSize + 16] < 1.0f, "the impulse peak is reduced") && ok;
    // Monotone falloff along the center row away from the peak (separable blur of an impulse is a
    // product of two Gaussians, so it decreases with distance).
    float previous = blurred[16 * kSize + 16];
    bool monotone = true;
    for (int offset = 1; offset <= 16; ++offset) {
        const float value = blurred[16 * kSize + 16 + static_cast<std::size_t>(offset)];
        monotone = monotone && value <= previous + 1.0e-6f;
        previous = value;
    }
    ok = expect(monotone, "the impulse falls off monotonically") && ok;
    // Symmetry: the two halves of a row match.
    ok = expect(near(blurred[16 * kSize + 15], blurred[16 * kSize + 17], 1.0e-6f),
                "the blur is symmetric") &&
         ok;
    return ok;
}

bool step_edge_is_smooth() {
    constexpr std::uint32_t kWidth = 16;
    constexpr std::uint32_t kHeight = 4;
    Image step(kWidth * kHeight, 0.0f);
    for (std::uint32_t y = 0; y < kHeight; ++y) {
        for (std::uint32_t x = 8; x < kWidth; ++x) {
            step[static_cast<std::size_t>(y) * kWidth + x] = 1.0f;
        }
    }
    const Image blurred = blur(step, kWidth, kHeight, 2.0f);
    bool monotone = true;
    float previous = blurred[1 * kWidth + 0];
    for (std::uint32_t x = 1; x < kWidth; ++x) {
        const float value = blurred[static_cast<std::size_t>(1) * kWidth + x];
        monotone = monotone && value >= previous - 1.0e-6f;
        previous = value;
    }
    bool ok = expect(monotone, "a blurred step stays monotone");
    ok = expect(blurred[1 * kWidth + 8] > 0.05f && blurred[1 * kWidth + 8] < 0.95f,
                "the step edge is soft, not binary") &&
         ok;
    // Determinism: the same input and sigma give byte-identical floats.
    ok = expect(blur(step, kWidth, kHeight, 2.0f) == blurred,
                "the reference blur is deterministic") &&
         ok;
    return ok;
}

bool limits() {
    GlassLimits limits;
    bool ok = expect(limits.max_blur_radius == 64.0f, "default radius clamp is 64 CSS px");
    ok = expect(limits.max_backdrop_area == 4'000'000, "default backdrop area is 4 Mpx") && ok;
    ok = expect(clamp_is(-5.0f, limits, 0.0f), "a negative sigma clamps to 0") && ok;
    ok = expect(clamp_is(20.0f, limits, 20.0f), "a sigma under the clamp is kept") && ok;
    ok = expect(clamp_is(100.0f, limits, 64.0f), "a sigma over the clamp is clamped") && ok;
    ok = expect(clamp_is(std::nanf(""), limits, 0.0f), "a non-finite sigma clamps to 0") && ok;
    limits.max_blur_radius = 12.0f;
    ok = expect(clamp_is(100.0f, limits, 12.0f), "the configured clamp is used") && ok;
    return ok;
}

bool plans() {
    GlassLimits limits;
    bool ok = true;

    const GlassBackdropPlan small = plan_glass_backdrop(Extent2D{100, 50}, 4.0f, 1.0f, limits);
    ok = expect(!small.half_resolution, "a small region blurs at full resolution") && ok;
    ok = expect(small.kernel.active() && small.blur_width == small.region_width &&
                    small.blur_height == small.region_height,
                "full resolution: blur target equals the region") &&
         ok;
    // Region = box + 3 sigma on every side.
    ok = expect(small.region_width == 100 + 2 * 12 && small.region_height == 50 + 2 * 12,
                "the dependency region grows by 3 sigma") &&
         ok;

    limits.max_backdrop_area = 1000;
    const GlassBackdropPlan large = plan_glass_backdrop(Extent2D{100, 50}, 4.0f, 1.0f, limits);
    ok = expect(large.half_resolution, "a region over the area limit blurs at half resolution") &&
         ok;
    ok = expect(large.blur_width == (large.region_width + 1) / 2 &&
                    large.blur_height == (large.region_height + 1) / 2,
                "half resolution rounds the blur target up") &&
         ok;
    ok = expect(near(large.kernel.sigma, small.kernel.sigma * 0.5f, 1.0e-6f),
                "half resolution halves the sigma in blur-target texels") &&
         ok;

    const GlassBackdropPlan empty = plan_glass_backdrop(Extent2D{0, 0}, 4.0f, 1.0f, limits);
    ok = expect(!empty.kernel.active() && empty.blur_width == 0, "an empty box has no plan") && ok;
    const GlassBackdropPlan none = plan_glass_backdrop(Extent2D{10, 10}, 0.0f, 1.0f, limits);
    ok = expect(!none.kernel.active(), "sigma 0 has no plan") && ok;

    // Device scale multiplies both limits and the region.
    const GlassBackdropPlan scaled = plan_glass_backdrop(Extent2D{10, 10}, 4.0f, 2.0f, limits);
    ok = expect(scaled.region_width == 10 + 2 * 24, "device scale scales the reach") && ok;
    return ok;
}

bool dependency_rects() {
    const Rect box{10.0f, 20.0f, 100.0f, 50.0f};
    bool ok = expect(glass_dependency_rect(box, 4.0f, 1.0f).x == -2.0f,
                     "the dependency region starts 3 sigma left of the box");
    const Rect region = glass_dependency_rect(box, 4.0f, 2.0f);
    ok = expect(region.x == 10.0f - 24.0f && region.y == 20.0f - 24.0f &&
                    region.width == 100.0f + 48.0f && region.height == 50.0f + 48.0f,
                "device scale multiplies the reach") &&
         ok;
    const Rect none = glass_dependency_rect(box, 0.0f, 1.0f);
    ok = expect(none.x == box.x && none.width == box.width, "no glass keeps the box") && ok;
    return ok;
}

} // namespace

int main() {
    bool ok = kernel_shape();
    ok = kernel_normalized() && ok;
    ok = constant_input_is_unchanged() && ok;
    ok = impulse_spreads_and_conserves() && ok;
    ok = step_edge_is_smooth() && ok;
    ok = limits() && ok;
    ok = plans() && ok;
    ok = dependency_rects() && ok;
    if (!ok) {
        std::cerr << "glass_blur: FAILED\n";
        return 1;
    }
    std::cout << "glass_blur: ok\n";
    return 0;
}
