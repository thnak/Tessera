// SPDX-License-Identifier: MIT

#include "paint_filter.hpp"

#include <algorithm>

namespace tessera::ui::paint::detail {
namespace {

using rendering::gpu::LayerFilterChain;
using rendering::gpu::LayerFilterOp;

// The Gaussian's reach: 3 sigma holds all but 0.3% of the weight, which is also the kernel radius
// the renderer runs (glass_blur.hpp make_glass_kernel).
constexpr float kSigmaReach = 3.0f;

// The style layer and the renderer size the same chain; a mismatch would reject or truncate valid
// declarations silently.
static_assert(style::kMaxFilterOps == rendering::gpu::kMaxLayerFilterOps);
static_assert(style::kColorFunctionsPerOp == rendering::gpu::kColorFunctionsPerOp);

rendering::gpu::ColorFunction color_function_of(style::FilterFunction::Kind kind) {
    using Kind = style::FilterFunction::Kind;
    using rendering::gpu::ColorFunction;
    switch (kind) {
    case Kind::Brightness:
        return ColorFunction::Brightness;
    case Kind::Contrast:
        return ColorFunction::Contrast;
    case Kind::Grayscale:
        return ColorFunction::Grayscale;
    case Kind::Sepia:
        return ColorFunction::Sepia;
    case Kind::Saturate:
        return ColorFunction::Saturate;
    case Kind::HueRotate:
        return ColorFunction::HueRotate;
    case Kind::Invert:
        return ColorFunction::Invert;
    case Kind::Opacity:
        return ColorFunction::Opacity;
    default:
        return ColorFunction::None;
    }
}

constexpr float kDegreesToRadians = 3.14159265358979323846f / 180.0f;

} // namespace

LayerFilterChain layer_filter_chain(const style::ComputedStyle& style,
                                    const rhi::Color& current_color,
                                    const rendering::gpu::GlassLimits& limits) {
    LayerFilterChain chain;
    const std::optional<style::Filter> filter = filter_value(style);
    if (!filter.has_value()) {
        return chain;
    }
    // Adjacent colour functions share one op (up to three), applied in order by one pass.
    std::size_t run = 0;
    for (std::size_t index = 0; index < filter->count; ++index) {
        const style::FilterFunction& function = filter->functions[index];
        if (function.is_color_function()) {
            if (run == 0 || run == rendering::gpu::kColorFunctionsPerOp) {
                chain.push(
                    LayerFilterOp{0.0f, 0.0f, 0.0f, 0u, LayerFilterOp::Kind::ColorFunctions});
                run = 0;
            }
            const float amount = function.kind == style::FilterFunction::Kind::HueRotate
                                     ? function.amount * kDegreesToRadians
                                     : function.amount;
            chain.ops[chain.count - 1].set_color_function(run++, color_function_of(function.kind),
                                                          amount);
            continue;
        }
        run = 0;
        LayerFilterOp op;
        op.sigma = rendering::gpu::clamp_glass_sigma(function.sigma, limits);
        if (function.kind == style::FilterFunction::Kind::DropShadow) {
            op.kind = LayerFilterOp::Kind::DropShadow;
            op.offset_x = function.offset_x;
            op.offset_y = function.offset_y;
            op.set_tint(function.current_color ? current_color
                                               : rhi::Color{function.color.r, function.color.g,
                                                            function.color.b, function.color.a});
        }
        chain.push(op);
    }
    return chain;
}

ShadowOutsets filter_outsets(const style::ComputedStyle& style,
                             const rendering::gpu::GlassLimits& limits) {
    ShadowOutsets outsets;
    const LayerFilterChain chain = layer_filter_chain(style, rhi::Color{}, limits);
    for (std::size_t index = 0; index < chain.count; ++index) {
        const LayerFilterOp& op = chain.ops[index];
        if (op.kind == LayerFilterOp::Kind::ColorFunctions) {
            continue; // a colour function moves no pixel
        }
        const float reach = kSigmaReach * op.sigma;
        outsets.blurred = outsets.blurred || op.sigma > 0.0f;
        if (op.kind == LayerFilterOp::Kind::Blur) {
            outsets.left += reach;
            outsets.right += reach;
            outsets.top += reach;
            outsets.bottom += reach;
            continue;
        }
        outsets.left += std::max(0.0f, reach - op.offset_x);
        outsets.right += std::max(0.0f, reach + op.offset_x);
        outsets.top += std::max(0.0f, reach - op.offset_y);
        outsets.bottom += std::max(0.0f, reach + op.offset_y);
    }
    return outsets;
}

} // namespace tessera::ui::paint::detail
