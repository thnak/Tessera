// SPDX-License-Identifier: MIT

#include "tessera/rendering/redraw_plan.hpp"

#include <iostream>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::make_redraw_plan;
using tessera::rendering::RedrawPlan;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::Scissor;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "redraw_plan: " << message << "\n";
    }
    return condition;
}

bool scissor_is(const Scissor& scissor, std::int32_t x, std::int32_t y, std::uint32_t width,
                std::uint32_t height) {
    return scissor.x == x && scissor.y == y && scissor.width == width && scissor.height == height;
}

} // namespace

int main() {
    DamageAccumulator empty_damage(DamageConfig{Extent2D{100, 100}, 4});
    const RedrawPlan empty_plan = make_redraw_plan(empty_damage);
    if (!expect(!empty_plan.full_redraw, "empty not full") ||
        !expect(empty_plan.scissors.empty(), "empty scissors")) {
        return 1;
    }

    DamageAccumulator damage(DamageConfig{Extent2D{100, 100}, 4});
    damage.add(Rect{10.2f, 20.7f, 30.1f, 40.3f});
    const RedrawPlan plan = make_redraw_plan(damage);
    if (!expect(!plan.full_redraw, "not full") ||
        !expect(plan.scissors.size() == 1, "one scissor") ||
        !expect(scissor_is(plan.scissors[0], 10, 20, 31, 41), "rounded scissor")) {
        return 2;
    }

    DamageAccumulator exact(DamageConfig{Extent2D{100, 100}, 4});
    exact.add(Rect{0, 0, 10, 10});
    const RedrawPlan exact_plan = make_redraw_plan(exact);
    if (!expect(scissor_is(exact_plan.scissors[0], 0, 0, 10, 10), "exact scissor")) {
        return 3;
    }

    DamageAccumulator clipped(DamageConfig{Extent2D{100, 100}, 4});
    clipped.add(Rect{-5, -5, 10, 10});
    const RedrawPlan clipped_plan = make_redraw_plan(clipped);
    if (!expect(clipped_plan.scissors.size() == 1, "clipped scissor") ||
        !expect(scissor_is(clipped_plan.scissors[0], 0, 0, 5, 5), "clipped top left")) {
        return 4;
    }

    DamageAccumulator edge(DamageConfig{Extent2D{100, 100}, 4});
    edge.add(Rect{95.2f, 95.2f, 10, 10});
    const RedrawPlan edge_plan = make_redraw_plan(edge);
    if (!expect(scissor_is(edge_plan.scissors[0], 95, 95, 5, 5), "clipped bottom right")) {
        return 5;
    }

    DamageAccumulator capped(DamageConfig{Extent2D{100, 100}, 1});
    capped.add(Rect{0, 0, 1, 1});
    capped.add(Rect{50, 50, 1, 1});
    const RedrawPlan capped_plan = make_redraw_plan(capped);
    if (!expect(capped_plan.full_redraw, "full redraw flag") ||
        !expect(capped_plan.scissors.size() == 1, "full scissor") ||
        !expect(scissor_is(capped_plan.scissors[0], 0, 0, 100, 100), "full bounds")) {
        return 6;
    }

    DamageAccumulator zero(DamageConfig{Extent2D{0, 0}, 4});
    zero.add(Rect{0, 0, 10, 10});
    const RedrawPlan zero_plan = make_redraw_plan(zero);
    if (!expect(!zero_plan.full_redraw, "zero target not full") ||
        !expect(zero_plan.scissors.empty(), "zero target empty")) {
        return 7;
    }

    const RedrawPlan repeat = make_redraw_plan(damage);
    if (!expect(repeat.scissors.size() == plan.scissors.size(), "deterministic size") ||
        !expect(scissor_is(repeat.scissors[0], 10, 20, 31, 41), "deterministic scissor")) {
        return 8;
    }

    std::cout << "redraw_plan: ok\n";
    return 0;
}
