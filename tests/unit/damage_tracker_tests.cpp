// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/damage_pass.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
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

    std::cout << "damage_tracker: ok\n";
    return 0;
}
