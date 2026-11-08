// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/damage_pass.hpp"

#include <cmath>
#include <iostream>
#include <memory>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::layout::add_scene_damage;
using tessera::ui::layout::DamagePassResult;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "damage_pass: " << message << "\n";
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

} // namespace

int main() {
    Node root(1);
    auto child = std::make_unique<Node>(2);
    child->set_bounds({10.1f, 20.2f, 30.3f, 40.4f});
    Node& child_ref = root.add_child(std::move(child));
    clear_tree(root);

    child_ref.mark_dirty(Dirty::Paint);
    DirtyPass dirty;
    dirty.collect(root, Dirty::Paint | Dirty::Render);
    DamageAccumulator damage(DamageConfig{Extent2D{200, 200}, 4});
    const DamagePassResult result = add_scene_damage(dirty, damage, 1.0f);
    if (!expect(result.nodes == 1, "one dirty node") ||
        !expect(result.rects == 1, "one damage rect") ||
        !expect(!result.full_redraw, "no full redraw") ||
        !expect(damage.rects().size() == 1, "damage stored") ||
        !expect(rect_is(damage.rects()[0], 10, 20, 30, 41), "snapped world bounds")) {
        return 1;
    }

    Node merge_root(10);
    auto left = std::make_unique<Node>(11);
    left->set_bounds({0, 0, 10, 10});
    Node& left_ref = merge_root.add_child(std::move(left));
    auto right = std::make_unique<Node>(12);
    right->set_bounds({8, 0, 10, 10});
    Node& right_ref = merge_root.add_child(std::move(right));
    clear_tree(merge_root);
    left_ref.mark_dirty(Dirty::Paint);
    right_ref.mark_dirty(Dirty::Render);

    DirtyPass merge_dirty;
    merge_dirty.collect(merge_root, Dirty::Paint | Dirty::Render);
    DamageAccumulator merged(DamageConfig{Extent2D{100, 100}, 4});
    const DamagePassResult merge_result = add_scene_damage(merge_dirty, merged, 1.0f);
    if (!expect(merge_result.nodes == 2, "two dirty nodes") ||
        !expect(merged.rects().size() == 1, "overlap merges") ||
        !expect(rect_is(merged.rects()[0], 0, 0, 18, 10), "merged damage")) {
        return 2;
    }

    Node layout_root(20);
    auto layout_child = std::make_unique<Node>(21);
    layout_child->set_bounds({0, 0, 20, 20});
    Node& layout_ref = layout_root.add_child(std::move(layout_child));
    clear_tree(layout_root);
    layout_ref.mark_dirty(Dirty::Layout);

    DirtyPass layout_dirty;
    layout_dirty.collect(layout_root, Dirty::Paint | Dirty::Render);
    DamageAccumulator layout_damage(DamageConfig{Extent2D{100, 100}, 4});
    const DamagePassResult layout_result = add_scene_damage(layout_dirty, layout_damage, 1.0f);
    if (!expect(layout_result.nodes == 0, "layout-only ignored") ||
        !expect(layout_damage.empty(), "no damage for layout-only")) {
        return 3;
    }

    Node scale_root(30);
    auto fractional = std::make_unique<Node>(31);
    fractional->set_bounds({0.1f, 0.1f, 1.0f, 1.0f});
    Node& fractional_ref = scale_root.add_child(std::move(fractional));
    clear_tree(scale_root);
    fractional_ref.mark_dirty(Dirty::Paint);

    DirtyPass scale_dirty;
    scale_dirty.collect(scale_root, Dirty::Paint | Dirty::Render);
    DamageAccumulator scaled(DamageConfig{Extent2D{100, 100}, 4});
    const DamagePassResult scale_result = add_scene_damage(scale_dirty, scaled, 1.25f);
    if (!expect(scale_result.rects == 1, "scaled damage count") ||
        !expect(rect_is(scaled.rects()[0], 0, 0, 0.8f, 0.8f), "scaled damage")) {
        return 4;
    }

    Node cap_root(40);
    auto first = std::make_unique<Node>(41);
    first->set_bounds({0, 0, 5, 5});
    Node& first_ref = cap_root.add_child(std::move(first));
    auto second = std::make_unique<Node>(42);
    second->set_bounds({50, 50, 5, 5});
    Node& second_ref = cap_root.add_child(std::move(second));
    clear_tree(cap_root);
    first_ref.mark_dirty(Dirty::Paint);
    second_ref.mark_dirty(Dirty::Paint);

    DirtyPass cap_dirty;
    cap_dirty.collect(cap_root, Dirty::Paint | Dirty::Render);
    DamageAccumulator capped(DamageConfig{Extent2D{100, 100}, 1});
    const DamagePassResult cap_result = add_scene_damage(cap_dirty, capped, 1.0f);
    if (!expect(cap_result.full_redraw, "cap falls back to full redraw") ||
        !expect(cap_result.rects == 0, "cap clears rects")) {
        return 5;
    }

    Node overflow_root(50);
    auto overflow_a = std::make_unique<Node>(51);
    overflow_a->set_bounds({0, 0, 5, 5});
    Node& overflow_a_ref = overflow_root.add_child(std::move(overflow_a));
    auto overflow_b = std::make_unique<Node>(52);
    overflow_b->set_bounds({50, 50, 5, 5});
    Node& overflow_b_ref = overflow_root.add_child(std::move(overflow_b));
    clear_tree(overflow_root);
    overflow_a_ref.mark_dirty(Dirty::Paint);
    overflow_b_ref.mark_dirty(Dirty::Paint);

    DirtyPass overflow_dirty(1);
    overflow_dirty.collect(overflow_root, Dirty::Paint | Dirty::Render);
    DamageAccumulator overflow_damage(DamageConfig{Extent2D{100, 100}, 4});
    const DamagePassResult overflow_result =
        add_scene_damage(overflow_dirty, overflow_damage, 1.0f);
    if (!expect(overflow_result.overflowed, "dirty overflow reported")) {
        return 6;
    }

    std::cout << "damage_pass: ok\n";
    return 0;
}
