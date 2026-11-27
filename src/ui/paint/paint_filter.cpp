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

} // namespace

LayerFilterChain layer_filter_chain(const style::ComputedStyle& style,
                                    const rhi::Color& current_color,
                                    const rendering::gpu::GlassLimits& limits) {
    LayerFilterChain chain;
    const std::optional<style::Filter> filter = filter_value(style);
    if (!filter.has_value()) {
        return chain;
    }
    for (std::size_t index = 0; index < filter->count; ++index) {
        const style::FilterFunction& function = filter->functions[index];
        LayerFilterOp op;
        op.sigma = rendering::gpu::clamp_glass_sigma(function.sigma, limits);
        if (function.kind == style::FilterFunction::Kind::DropShadow) {
            op.kind = LayerFilterOp::Kind::DropShadow;
            op.offset_x = function.offset_x;
            op.offset_y = function.offset_y;
            op.color = function.current_color ? current_color
                                              : rhi::Color{function.color.r, function.color.g,
                                                           function.color.b, function.color.a};
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
