// SPDX-License-Identifier: MIT

#include "tessera/rendering/redraw_plan.hpp"
#include "tessera/ui/layout/damage_pass.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::make_redraw_plan;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::layout::DamagePassResult;
using tessera::ui::layout::DamageTracker;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "damage_tracker: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

bool rect_is(const Rect& rect, float x, float y, float width, float height) {
    return near(rect.x, x) && near(rect.y, y) && near(rect.width, width) &&
           near(rect.height, height);
}

void clear_tree(Node& root) {
    root.clear_dirty(Dirty::All);
    for (const auto& child : root.children()) {
        clear_tree(*child);
    }
}

bool contains(const DamageAccumulator& damage, float x, float y, float width, float height) {
    for (const Rect& rect : damage.rects()) {
        if (rect_is(rect, x, y, width, height)) {
            return true;
        }
    }
    return false;
}

void damage_node(Node& root, Node& child, DamageTracker& tracker, DamageAccumulator& damage) {
    clear_tree(root);
    child.mark_dirty(Dirty::Paint);
    DirtyPass pass;
    pass.collect(root, Dirty::Paint | Dirty::Render);
    tracker.add_scene_damage(pass, damage, 1.0f);
}

} // namespace

int main() {
    Node root(1);
    auto child = std::make_unique<Node>(2);
    child->set_bounds({10, 20, 30, 40});
    Node& child_ref = root.add_child(std::move(child));

    DamageTracker tracker;
    DamageAccumulator damage(DamageConfig{Extent2D{300, 200}, 8});
    damage_node(root, child_ref, tracker, damage);
    if (!expect(tracker.entries() == 1, "initial entry") ||
        !expect(damage.rects().size() == 1, "initial damage") ||
        !expect(rect_is(damage.rects()[0], 10, 20, 30, 40), "initial bounds")) {
        return 1;
    }

    child_ref.set_bounds({100, 50, 30, 40});
    damage_node(root, child_ref, tracker, damage);
    if (!expect(tracker.entries() == 1, "still one entry") ||
        !expect(damage.rects().size() == 2, "old and new damage") ||
        !expect(contains(damage, 10, 20, 30, 40), "old rect preserved") ||
        !expect(contains(damage, 100, 50, 30, 40), "new rect added") ||
        !expect(tracker.previous_bounds(2).has_value(), "previous bounds present") ||
        !expect(rect_is(*tracker.previous_bounds(2), 100, 50, 30, 40), "previous updated")) {
        return 2;
    }

    damage_node(root, child_ref, tracker, damage);
    if (!expect(damage.rects().size() == 2, "same bounds still one damage rect")) {
        return 3;
    }

    Node prune_root(99);
    if (!expect(tracker.prune_removed(prune_root) == 1, "prune removed node") ||
        !expect(tracker.entries() == 0, "prune shrinks entries") ||
        !expect(!tracker.previous_bounds(2).has_value(), "pruned previous gone")) {
        return 4;
    }

    Node retention_root(10);
    auto first = std::make_unique<Node>(11);
    first->set_bounds({0, 0, 10, 10});
    Node& first_ref = retention_root.add_child(std::move(first));
    auto second = std::make_unique<Node>(12);
    second->set_bounds({50, 50, 10, 10});
    Node& second_ref = retention_root.add_child(std::move(second));

    DamageTracker bounded(1);
    DamageAccumulator bounded_damage(DamageConfig{Extent2D{100, 100}, 4});
    damage_node(retention_root, first_ref, bounded, bounded_damage);
    damage_node(retention_root, second_ref, bounded, bounded_damage);
    if (!expect(bounded.entries() == 1, "retention cap") ||
        !expect(bounded.previous_bounds(11) == std::nullopt, "old entry evicted") ||
        !expect(bounded.previous_bounds(12).has_value(), "new entry retained")) {
        return 5;
    }

    bounded.clear();
    if (!expect(bounded.entries() == 0, "clear entries")) {
        return 6;
    }

    bool threw = false;
    try {
        DamageTracker invalid(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "zero capacity rejected")) {
        return 7;
    }

    // Dirty-pass overflow through the tracker path must also force a full-target redraw.
    Node overflow_root(90);
    auto overflow_a = std::make_unique<Node>(91);
    overflow_a->set_bounds({0, 0, 5, 5});
    Node& overflow_a_ref = overflow_root.add_child(std::move(overflow_a));
    auto overflow_b = std::make_unique<Node>(92);
    overflow_b->set_bounds({20, 0, 5, 5});
    Node& overflow_b_ref = overflow_root.add_child(std::move(overflow_b));
    auto overflow_c = std::make_unique<Node>(93);
    overflow_c->set_bounds({40, 0, 5, 5});
    Node& overflow_c_ref = overflow_root.add_child(std::move(overflow_c));
    clear_tree(overflow_root);
    overflow_a_ref.mark_dirty(Dirty::Paint);
    overflow_b_ref.mark_dirty(Dirty::Paint);
    overflow_c_ref.mark_dirty(Dirty::Paint);

    DamageTracker overflow_tracker;
    DamageAccumulator overflow_damage(DamageConfig{Extent2D{100, 100}, 8});
    DirtyPass overflow_pass(2);
    overflow_pass.collect(overflow_root, Dirty::Paint | Dirty::Render);
    const DamagePassResult overflow_result =
        overflow_tracker.add_scene_damage(overflow_pass, overflow_damage, 1.0f);
    if (!expect(overflow_result.overflowed, "tracker overflow reported") ||
        !expect(overflow_result.full_redraw, "tracker overflow forces full damage") ||
        !expect(overflow_damage.full_redraw(), "tracker overflow latches the accumulator") ||
        !expect(make_redraw_plan(overflow_damage).full_redraw, "tracker overflow plan is full")) {
        return 20;
    }
    // ...and the latch clears so later frames go back to partial damage.
    overflow_damage.clear();
    overflow_damage.add(Rect{5, 5, 5, 5});
    if (!expect(!make_redraw_plan(overflow_damage).full_redraw,
                "tracker overflow recovers after clear")) {
        return 21;
    }

    std::cout << "damage_tracker: ok\n";
    return 0;
}
