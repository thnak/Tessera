// SPDX-License-Identifier: MIT
//
// Pure rounded-clip math: radius clamping, conservative nesting intersection, containment, and the
// degenerate cases (zero radius, radius >= half the box, empty intersections). No GPU/backend.

#include "tessera/rendering/gpu/clip.hpp"

#include <cmath>
#include <iostream>

namespace {

using tessera::rendering::gpu::clip::clamp_radii;
using tessera::rendering::gpu::clip::enclosing_scissor;
using tessera::rendering::gpu::clip::intersect_clips;
using tessera::rendering::gpu::clip::make_rounded_rect;
using tessera::rendering::gpu::clip::RoundedRect;
using tessera::rendering::gpu::clip::uniform_rect;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::Scissor;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "clip_math: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected) {
    return std::abs(value - expected) <= 1.0e-4f;
}

bool rect_is(const Rect& rect, float x, float y, float width, float height) {
    return near(rect.x, x) && near(rect.y, y) && near(rect.width, width) &&
           near(rect.height, height);
}

bool radii_are(const RoundedRect& clip, float top_left, float top_right, float bottom_right,
               float bottom_left) {
    return near(clip.top_left, top_left) && near(clip.top_right, top_right) &&
           near(clip.bottom_right, bottom_right) && near(clip.bottom_left, bottom_left);
}

bool scissor_is(const Scissor& scissor, std::int32_t x, std::int32_t y, std::uint32_t width,
                std::uint32_t height) {
    return scissor.x == x && scissor.y == y && scissor.width == width && scissor.height == height;
}

} // namespace

int main() {
    // Radius clamping: negative -> 0, oversized -> half the smaller dimension.
    const RoundedRect clamped = make_rounded_rect({0, 0, 40, 20}, -4.0f, 100.0f, 10.0f, 0.0f);
    bool ok = expect(rect_is(clamped.rect, 0, 0, 40, 20), "clamp keeps the rect") &&
              expect(radii_are(clamped, 0.0f, 10.0f, 10.0f, 0.0f),
                     "clamp to half the smaller dimension, negative to zero") &&
              expect(clamped.rounded(), "mixed radii are rounded");

    const RoundedRect square = clamp_radii(RoundedRect{{0, 0, 10, 10}, 5.0f, 6.0f, 7.0f, 8.0f});
    ok = expect(radii_are(square, 5.0f, 5.0f, 5.0f, 5.0f), "radius >= half clamps to half") && ok;
    ok = expect(!uniform_rect({0, 0, 10, 10}, 0.0f).rounded(), "zero radii are rectangular") && ok;
    ok = expect(!uniform_rect({0, 0, 0, 10}, 4.0f).has_area(), "zero-width clip has no area") && ok;

    // Containment: a rounded clip strictly inside a rectangular one keeps its corners when its
    // rect edges coincide with the intersection.
    const RoundedRect inner = uniform_rect({10, 10, 40, 40}, 8.0f);
    const RoundedRect outer_rect = uniform_rect({0, 0, 100, 100}, 0.0f);
    const RoundedRect contained = intersect_clips(outer_rect, inner);
    ok = expect(rect_is(contained.rect, 10, 10, 40, 40), "contained intersection rect") && ok;
    ok =
        expect(radii_are(contained, 8.0f, 8.0f, 8.0f, 8.0f), "contained rounded corners survive") &&
        ok;

    // A straight edge cut squares only the cut corners; the surviving corners keep their radius.
    const RoundedRect cut = intersect_clips(uniform_rect({20, 0, 100, 100}, 0.0f), inner);
    ok = expect(rect_is(cut.rect, 20, 10, 30, 40), "cut intersection rect") && ok;
    ok = expect(radii_are(cut, 0.0f, 8.0f, 8.0f, 0.0f), "edge cuts square the cut corners only") &&
         ok;

    // Rounded-in-rounded: coincident corners take the tighter (larger) radius conservatively.
    const RoundedRect tighter =
        intersect_clips(uniform_rect({0, 0, 100, 100}, 8.0f), uniform_rect({0, 0, 50, 50}, 20.0f));
    ok = expect(rect_is(tighter.rect, 0, 0, 50, 50), "rounded-in-rounded rect") && ok;
    ok = expect(radii_are(tighter, 20.0f, 20.0f, 20.0f, 20.0f),
                "rounded-in-rounded takes the tighter radius") &&
         ok;

    // Offset rounded-in-rounded (contained, no cut): the inner radii survive.
    const RoundedRect offset = intersect_clips(uniform_rect({0, 0, 100, 100}, 10.0f),
                                               uniform_rect({10, 10, 80, 80}, 4.0f));
    ok = expect(rect_is(offset.rect, 10, 10, 80, 80), "offset intersection rect") && ok;
    ok = expect(radii_are(offset, 4.0f, 4.0f, 4.0f, 4.0f), "contained inner radii") && ok;

    // Disjoint clips collapse to an empty clip.
    const RoundedRect disjoint =
        intersect_clips(uniform_rect({0, 0, 10, 10}, 0.0f), uniform_rect({20, 20, 10, 10}, 0.0f));
    ok = expect(!disjoint.has_area(), "disjoint clips are empty") && ok;

    // The scissor fast path is the smallest integer rect containing the fractional clip rect.
    const Scissor scissor = enclosing_scissor(uniform_rect({1.2f, 2.4f, 3.3f, 4.1f}, 0.0f));
    ok = expect(scissor_is(scissor, 1, 2, 4, 5), "enclosing scissor rounds outward") && ok;
    ok = expect(scissor_is(enclosing_scissor(RoundedRect{}), 0, 0, 0, 0),
                "empty clip has an empty scissor") &&
         ok;

    ok = expect(uniform_rect({0, 0, 10, 10}, 3.0f) == uniform_rect({0, 0, 10, 10}, 3.0f),
                "identical clips compare equal") &&
         ok;
    ok = expect(!(uniform_rect({0, 0, 10, 10}, 3.0f) == uniform_rect({0, 0, 10, 10}, 4.0f)),
                "different radii compare unequal") &&
         ok;

    if (!ok) {
        std::cerr << "clip_math: failed\n";
        return 1;
    }
    std::cout << "clip_math: ok\n";
    return 0;
}
