// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/gpu/gradient_records.hpp"
#include "tessera/rendering/rhi/types.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace tessera::rendering::gpu::gradient {

// One colour stop as the renderer takes it. Offsets are sorted ascending and are positions along
// the ramp parameter t, not confined to [0, 1]; the colour's alpha already includes the node
// opacity: the paint pass does both before it reaches a sink, so the renderer and the shader never
// see an unresolved stop.
struct Stop {
    float offset = 0.0f;
    rhi::Color color{0.0f, 0.0f, 0.0f, 0.0f};
};

// One gradient layer: the quad it fills and the geometry the record encodes. The centre is in the
// same absolute paint coordinates as `rect`; `pack` turns it into quad-local space.
struct Fill {
    Kind kind = Kind::Linear;
    rhi::Rect rect;
    float angle_degrees = 180.0f; // linear
    float center_x = 0.0f;        // radial and conic
    float center_y = 0.0f;
    float radius_x = 0.0f; // radial
    float radius_y = 0.0f;
    float from_angle_degrees = 0.0f; // conic, clockwise from 12 o'clock
    bool repeating = false;          // wrap t over [first stop, last stop) (kFlagRepeating)
    std::span<const Stop> stops;
};

// A record plus its ramp, in the layout the shader reads, without a table. The CPU reference packs
// into one of these and evaluates it with the same `evaluate` below, so the CPU and the GPU start
// from the same bytes and only the shader's arithmetic is left to disagree.
struct Packed {
    GradientRecord record;
    std::array<float, kMaxStopsPerGradient * 4> colors{};
    std::array<float, kMaxStopsPerGradient> offsets{};
};

// Fills `out` from `fill`; false when the fill cannot be drawn (no stops or more than the record
// holds, an empty quad, a radial with a non-positive radius, or a non-finite parameter). The caller
// then paints nothing for that layer, which is what the banded path did for the same input.
[[nodiscard]] inline bool pack(const Fill& fill, Packed& out) {
    const std::size_t count = fill.stops.size();
    if (count == 0 || count > kMaxStopsPerGradient || !(fill.rect.width > 0.0f) ||
        !(fill.rect.height > 0.0f)) {
        return false;
    }
    GradientRecord& record = out.record;
    record = GradientRecord{};
    record.kind = static_cast<std::uint32_t>(fill.kind);
    // Only a ramp with a positive period can repeat; anything else holds its end stops as a plain
    // one does, which is also what the shader's divide-by-period needs to stay finite.
    if (fill.repeating && fill.stops.back().offset > fill.stops.front().offset) {
        record.flags |= kFlagRepeating;
    }
    record.stop_count = static_cast<std::uint32_t>(count);
    record.first_stop = 0;
    const float centre_x = fill.rect.x + fill.rect.width * 0.5f;
    const float centre_y = fill.rect.y + fill.rect.height * 0.5f;
    switch (fill.kind) {
    case Kind::Linear: {
        const float radians = fill.angle_degrees * 3.14159265358979323846f / 180.0f;
        const float dx = std::sin(radians);
        const float dy = -std::cos(radians);
        // The gradient line's length across the box (CSS Images 3 section 3.1.1), at least a pixel
        // so a degenerate box cannot divide by zero.
        const float span =
            std::max(fill.rect.width * std::abs(dx) + fill.rect.height * std::abs(dy), 1.0f);
        record.params[0] = dx / span;
        record.params[1] = dy / span;
        record.params[2] = 0.5f;
        break;
    }
    case Kind::Radial:
        if (!(fill.radius_x > 0.0f) || !(fill.radius_y > 0.0f)) {
            return false;
        }
        record.params[0] = fill.center_x - centre_x;
        record.params[1] = fill.center_y - centre_y;
        record.params[2] = fill.radius_x;
        record.params[3] = fill.radius_y;
        break;
    case Kind::Conic:
        record.params[0] = fill.center_x - centre_x;
        record.params[1] = fill.center_y - centre_y;
        record.params[2] = fill.from_angle_degrees / 360.0f;
        break;
    default:
        return false;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const Stop& stop = fill.stops[index];
        out.colors[index * 4 + 0] = stop.color.r;
        out.colors[index * 4 + 1] = stop.color.g;
        out.colors[index * 4 + 2] = stop.color.b;
        out.colors[index * 4 + 3] = stop.color.a;
        out.offsets[index] = stop.offset;
    }
    for (const float parameter : record.params) {
        if (!std::isfinite(parameter)) {
            return false;
        }
    }
    return true;
}

// The ramp's position at a quad-local point, `lx`/`ly` being pixels from the quad's centre. This is
// the shader's `gradient_t`, expression for expression.
[[nodiscard]] inline float position(const GradientRecord& record, float lx, float ly) {
    constexpr float kTau = 6.28318530717958647692f;
    switch (static_cast<Kind>(record.kind)) {
    case Kind::Radial: {
        const float nx = (lx - record.params[0]) / record.params[2];
        const float ny = (ly - record.params[1]) / record.params[3];
        return std::sqrt(nx * nx + ny * ny);
    }
    case Kind::Conic: {
        const float dx = lx - record.params[0];
        const float dy = ly - record.params[1];
        // The CSS angle runs clockwise from 12 o'clock, hence atan2(dx, -dy). The centre itself has
        // no angle and takes 0, which atan2 would leave undefined (and a GPU's atan too).
        const float angle = (dx == 0.0f && dy == 0.0f) ? 0.0f : std::atan2(dx, -dy);
        const float turn = angle / kTau - record.params[2];
        return turn - std::floor(turn);
    }
    case Kind::Linear:
    default:
        return lx * record.params[0] + ly * record.params[1] + record.params[2];
    }
}

// Encoded-space, premultiplied interpolation over sorted stops, the shader's `gradient_color`. The
// result is the same as `ui::paint::sample_gradient_stops` (a sweep test pins it); it is written so
// a shader can run it without an early return: find the last stop at or below t, then blend it with
// the next. At a duplicate offset the later stop wins, and t below the first stop or above the last
// takes that stop's colour.
[[nodiscard]] inline rhi::Color ramp(const float* colors, const float* offsets, std::uint32_t count,
                                     float t, bool repeating = false) {
    const auto stop = [colors](std::uint32_t index) {
        return rhi::Color{colors[index * 4 + 0], colors[index * 4 + 1], colors[index * 4 + 2],
                          colors[index * 4 + 3]};
    };
    if (count == 0) {
        return rhi::Color{0.0f, 0.0f, 0.0f, 0.0f};
    }
    if (count == 1) {
        return stop(0);
    }
    // The ramp holds its end stops (no clamp to 0..1: a stop may sit outside the line), or, for a
    // repeating ramp, wraps t into [first, last). The wrap is `sample_gradient_stops`'s.
    if (repeating) {
        const float first = offsets[0];
        const float period = offsets[count - 1] - first;
        if (period > 0.0f) {
            const float cycles = (t - first) / period;
            t = first + (cycles - std::floor(cycles)) * period;
        }
    }
    std::int32_t below = -1;
    for (std::uint32_t index = 0; index < count; ++index) {
        if (offsets[index] <= t) {
            below = static_cast<std::int32_t>(index);
        }
    }
    if (below < 0) {
        return stop(0);
    }
    const auto k = static_cast<std::uint32_t>(below);
    if (offsets[k] == t || k + 1 == count) {
        return stop(k);
    }
    const rhi::Color a = stop(k);
    const rhi::Color b = stop(k + 1);
    const float local = (t - offsets[k]) / (offsets[k + 1] - offsets[k]);
    // Premultiplied, then un-premultiplied (the blend stage composites straight alpha): CSS Images
    // 3.5.3 and Chromium. Opaque and same-hue ramps come out unchanged. The shader's
    // `gradient_color` is this block expression for expression.
    const float alpha = a.a + (b.a - a.a) * local;
    const float pr = a.r * a.a + (b.r * b.a - a.r * a.a) * local;
    const float pg = a.g * a.a + (b.g * b.a - a.g * a.a) * local;
    const float pb = a.b * a.a + (b.b * b.a - a.b * a.a) * local;
    if (alpha > 0.0f) {
        return rhi::Color{pr / alpha, pg / alpha, pb / alpha, alpha};
    }
    return rhi::Color{a.r + (b.r - a.r) * local, a.g + (b.g - a.g) * local,
                      a.b + (b.b - a.b) * local, alpha};
}

// The colour at a quad-local point: the whole per-fragment computation. `colors` and `offsets`
// point at the record's first stop.
[[nodiscard]] inline rhi::Color evaluate(const GradientRecord& record, const float* colors,
                                         const float* offsets, float lx, float ly) {
    return ramp(colors, offsets, std::min(record.stop_count, kMaxStopsPerGradient),
                position(record, lx, ly), (record.flags & kFlagRepeating) != 0);
}

[[nodiscard]] inline rhi::Color evaluate(const Packed& packed, float lx, float ly) {
    return evaluate(packed.record, packed.colors.data(), packed.offsets.data(), lx, ly);
}

} // namespace tessera::rendering::gpu::gradient
