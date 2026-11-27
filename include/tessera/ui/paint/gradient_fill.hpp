// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/gradient_math.hpp"
#include "tessera/ui/paint/paint_sink.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace tessera::ui::paint {

// A paint-pass gradient draw in the renderer's own terms (docs/design/gpu/gradients.md, ADR-076).
// Both the GPU sink and the CPU reference sink go through these, so the record the shader reads and
// the one the CPU evaluates start from the same fields and cannot be built two ways.
//
// `FillStops` owns the converted stops on the stack (at most kMaxDrawStops), so building a fill
// allocates nothing; the `gradient::Fill` it feeds borrows the stops and must not outlive it.
class FillStops {
public:
    FillStops(const LinearGradientStop* source, std::size_t stop_count)
        : count_(std::min(stop_count, kMaxDrawStops)) {
        for (std::size_t index = 0; index < count_; ++index) {
            stops_[index] =
                rendering::gpu::gradient::Stop{source[index].offset, source[index].color};
        }
    }

    [[nodiscard]] std::span<const rendering::gpu::gradient::Stop> view() const {
        return {stops_.data(), count_};
    }

private:
    std::array<rendering::gpu::gradient::Stop, kMaxDrawStops> stops_{};
    std::size_t count_ = 0;
};

[[nodiscard]] inline rendering::gpu::gradient::Fill linear_fill(const GradientDraw& draw,
                                                                const FillStops& stops) {
    rendering::gpu::gradient::Fill fill;
    fill.kind = rendering::gpu::gradient::Kind::Linear;
    fill.rect = draw.rect;
    fill.angle_degrees = draw.angle_degrees;
    fill.stops = stops.view();
    return fill;
}

[[nodiscard]] inline rendering::gpu::gradient::Fill radial_fill(const RadialGradientDraw& draw,
                                                                const FillStops& stops) {
    rendering::gpu::gradient::Fill fill;
    fill.kind = rendering::gpu::gradient::Kind::Radial;
    fill.rect = draw.rect;
    fill.center_x = draw.center_x;
    fill.center_y = draw.center_y;
    fill.radius_x = draw.radius_x;
    fill.radius_y = draw.radius_y;
    fill.stops = stops.view();
    return fill;
}

[[nodiscard]] inline rendering::gpu::gradient::Fill conic_fill(const ConicGradientDraw& draw,
                                                               const FillStops& stops) {
    rendering::gpu::gradient::Fill fill;
    fill.kind = rendering::gpu::gradient::Kind::Conic;
    fill.rect = draw.rect;
    fill.center_x = draw.center_x;
    fill.center_y = draw.center_y;
    fill.from_angle_degrees = draw.from_angle_degrees;
    fill.stops = stops.view();
    return fill;
}

} // namespace tessera::ui::paint
