// SPDX-License-Identifier: MIT
//
// Regression coverage for the damage accumulator's clear/latch contract (task-62): after clear()
// the accumulator must behave exactly like a freshly constructed one, so a whole-screen change
// cannot leave the next frame latched to full redraw and an idle frame stays empty.

#include "tessera/rendering/damage.hpp"
#include "tessera/rendering/redraw_plan.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::make_redraw_plan;
using tessera::rendering::RedrawPlan;
using tessera::rendering::should_full_redraw;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "damage_clear: " << message << "\n";
    }
    return condition;
}

bool near(float value, float expected) {
    return std::abs(value - expected) <= 1.0e-6f;
}

bool same_rect(const Rect& rect, float x, float y, float width, float height) {
    return near(rect.x, x) && near(rect.y, y) && near(rect.width, width) &&
           near(rect.height, height);
}

bool same_rects(const std::vector<Rect>& lhs, const std::vector<Rect>& rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (!same_rect(lhs[index], rhs[index].x, rhs[index].y, rhs[index].width,
                       rhs[index].height)) {
            return false;
        }
    }
    return true;
}

bool same_plan(const RedrawPlan& lhs, const RedrawPlan& rhs) {
    if (lhs.full_redraw != rhs.full_redraw || lhs.scissors.size() != rhs.scissors.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.scissors.size(); ++index) {
        const auto& left = lhs.scissors[index];
        const auto& right = rhs.scissors[index];
        if (left.x != right.x || left.y != right.y || left.width != right.width ||
            left.height != right.height) {
            return false;
        }
    }
    return true;
}

constexpr Extent2D kTarget{100, 100};

// A whole-target change makes the plan full through the area policy (not the accumulator latch).
bool clear_after_policy_full_is_partial() {
    DamageAccumulator damage{DamageConfig{kTarget, 4}};
    damage.add(Rect{0, 0, 100, 100});
    const RedrawPlan full_plan = make_redraw_plan(damage);
    if (!expect(!damage.full_redraw(),
                "a single whole-target rect does not latch the accumulator") ||
        !expect(should_full_redraw(full_plan, kTarget),
                "the area policy reports a full redraw for the whole target")) {
        return false;
    }

    damage.clear();
    if (!expect(damage.empty(), "clear empties the accumulator") ||
        !expect(!damage.full_redraw(), "clear resets the accumulator latch") ||
        !expect(make_redraw_plan(damage).scissors.empty(), "an idle plan is empty after clear") ||
        !expect(!should_full_redraw(make_redraw_plan(damage), kTarget),
                "an idle plan does not ask for a full redraw")) {
        return false;
    }

    damage.add(Rect{10, 10, 20, 20});
    const RedrawPlan partial = make_redraw_plan(damage);
    return expect(!partial.full_redraw, "small damage after clear is partial") &&
           expect(partial.scissors.size() == 1, "small damage after clear is one region") &&
           expect(partial.scissors[0].x == 10 && partial.scissors[0].y == 10 &&
                      partial.scissors[0].width == 20 && partial.scissors[0].height == 20,
                  "small damage after clear keeps its bounds") &&
           expect(!should_full_redraw(partial, kTarget),
                  "small damage after clear stays under the area policy");
}

// Clearing a latched (rect-cap) accumulator must also drop the latch.
bool clear_after_cap_latch_is_partial() {
    DamageAccumulator damage{DamageConfig{kTarget, 2}};
    damage.add(Rect{0, 0, 1, 1});
    damage.add(Rect{50, 50, 1, 1});
    damage.add(Rect{90, 90, 1, 1});
    if (!expect(damage.full_redraw(), "exceeding the rect cap latches a full redraw") ||
        !expect(damage.rects().empty(), "a latched accumulator drops its rects")) {
        return false;
    }

    damage.clear();
    if (!expect(!damage.full_redraw(), "clear resets the cap latch") ||
        !expect(damage.empty(), "cleared accumulator is empty")) {
        return false;
    }
    damage.clear();
    if (!expect(!damage.full_redraw(), "clearing twice keeps the latch clear")) {
        return false;
    }
    damage.add(Rect{5, 5, 10, 10});
    const RedrawPlan partial = make_redraw_plan(damage);
    return expect(!partial.full_redraw, "small damage after the cleared cap is partial") &&
           expect(partial.scissors.size() == 1, "one region after the cleared cap") &&
           expect(should_full_redraw(partial, kTarget) == false,
                  "the cleared cap does not force a full redraw");

    // The configured cap still applies after clear (clear resets state, not configuration).
}

bool clear_on_empty_and_repeated() {
    DamageAccumulator damage{DamageConfig{kTarget, 4}};
    damage.clear();
    damage.clear();
    if (!expect(damage.empty(), "clearing an empty accumulator keeps it empty") ||
        !expect(!damage.full_redraw(), "clearing an empty accumulator keeps the latch clear") ||
        !expect(make_redraw_plan(damage).scissors.empty(), "empty plan after repeated clear")) {
        return false;
    }
    damage.add(Rect{20, 20, 10, 10});
    damage.clear();
    if (!expect(damage.empty() && !damage.full_redraw(), "clear after one rect")) {
        return false;
    }
    damage.add(Rect{30, 30, 10, 10});
    return expect(damage.rects().size() == 1 && same_rect(damage.rects()[0], 30, 30, 10, 10),
                  "the accumulator is reusable after clear");
}

// A cleared accumulator must be indistinguishable from a fresh one across a scripted sequence.
bool cleared_matches_fresh() {
    DamageAccumulator dirty_first{DamageConfig{kTarget, 4}};
    dirty_first.add(Rect{0, 0, 100, 100});
    dirty_first.add(Rect{40, 40, 30, 30});
    dirty_first.clear();
    DamageAccumulator fresh{DamageConfig{kTarget, 4}};

    struct Step {
        bool clear = false;
        Rect rect{};
        bool has_rect = false;
    };
    const Step script[] = {
        {.clear = false, .rect = {10, 10, 20, 20}, .has_rect = true},
        {.clear = true},
        {.clear = false, .rect = {70, 70, 20, 20}, .has_rect = true},
        {.clear = false, .rect = {70, 80, 10, 10}, .has_rect = true}, // merges
        {.clear = false, .rect = {0, 0, 5, 5}, .has_rect = true},
        {.clear = true},
        {.clear = false, .rect = {0, 0, 100, 100}, .has_rect = true}, // policy full
        {.clear = true},
        {.clear = false, .rect = {25, 25, 10, 10}, .has_rect = true},
        {.clear = true},
    };
    for (const Step& step : script) {
        if (step.clear) {
            dirty_first.clear();
            fresh.clear();
            continue;
        }
        if (!step.has_rect) {
            continue;
        }
        dirty_first.add(step.rect);
        fresh.add(step.rect);
        const RedrawPlan left = make_redraw_plan(dirty_first);
        const RedrawPlan right = make_redraw_plan(fresh);
        if (!expect(same_rects(dirty_first.rects(), fresh.rects()),
                    "cleared accumulator rects equal a fresh accumulator") ||
            !expect(dirty_first.full_redraw() == fresh.full_redraw(),
                    "cleared accumulator latch equals a fresh accumulator") ||
            !expect(same_plan(left, right), "cleared accumulator plan equals a fresh plan") ||
            !expect(should_full_redraw(left, kTarget) == should_full_redraw(right, kTarget),
                    "cleared accumulator decision equals a fresh decision")) {
            return false;
        }
    }
    return true;
}

bool determinism() {
    const auto run = [](RedrawPlan& plan, std::vector<Rect>& rects, bool& latch) {
        DamageAccumulator damage{DamageConfig{kTarget, 3}};
        damage.add(Rect{0, 0, 100, 100});
        damage.clear();
        damage.add(Rect{10, 10, 20, 20});
        damage.add(Rect{10, 34, 20, 20});
        damage.add(Rect{10, 58, 20, 20});
        damage.add(Rect{10, 82, 20, 20}); // exceeds the cap
        plan = make_redraw_plan(damage);
        rects = damage.rects();
        latch = damage.full_redraw();
        damage.clear();
        damage.add(Rect{80, 80, 10, 10});
        plan = make_redraw_plan(damage);
        rects = damage.rects();
        latch = damage.full_redraw();
    };
    RedrawPlan first_plan;
    RedrawPlan second_plan;
    std::vector<Rect> first_rects;
    std::vector<Rect> second_rects;
    bool first_latch = false;
    bool second_latch = false;
    run(first_plan, first_rects, first_latch);
    run(second_plan, second_rects, second_latch);
    return expect(same_rects(first_rects, second_rects), "same rects across runs") &&
           expect(first_latch == second_latch, "same latch across runs") &&
           expect(same_plan(first_plan, second_plan), "same plan across runs");
}

} // namespace

int main() {
    if (!clear_after_policy_full_is_partial()) {
        return 1;
    }
    if (!clear_after_cap_latch_is_partial()) {
        return 2;
    }
    if (!clear_on_empty_and_repeated()) {
        return 3;
    }
    if (!cleared_matches_fresh()) {
        return 4;
    }
    if (!determinism()) {
        return 5;
    }

    std::cout << "damage_clear: ok\n";
    return 0;
}
