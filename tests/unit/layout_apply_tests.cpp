// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/apply_layout.hpp"

#include <cmath>
#include <iostream>
#include <memory>

namespace {

using tessera::ui::Dirty;
using tessera::ui::Node;
using tessera::ui::layout::LayoutApplyResult;
using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutResult;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layout_apply: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.01f;
}

bool rect_is(const Node& node, float x, float y, float width, float height) {
    const auto& bounds = node.bounds();
    return near(bounds.x, x) && near(bounds.y, y) && near(bounds.width, width) &&
           near(bounds.height, height);
}

bool any_paint_dirty(const Node& node) {
    return tessera::ui::has_any(node.dirty(), Dirty::Paint | Dirty::Render);
}

} // namespace

int main() {
    Node root(1);
    auto panel = std::make_unique<Node>(2);
    Node& panel_ref = root.add_child(std::move(panel));
    auto label = std::make_unique<Node>(3);
    Node& label_ref = panel_ref.add_child(std::move(label));

    root.clear_dirty(Dirty::All);
    panel_ref.clear_dirty(Dirty::All);
    label_ref.clear_dirty(Dirty::All);

    const LayoutResult layout{{LayoutBox{1, {0.0f, 0.0f, 400.0f, 300.0f}},
                               LayoutBox{2, {10.0f, 20.0f, 200.0f, 100.0f}},
                               LayoutBox{3, {15.0f, 27.0f, 50.0f, 10.0f}}}};
    const LayoutApplyResult applied = tessera::ui::layout::apply_layout(layout, root);
    if (!expect(applied.applied == 3, "applied count") ||
        !expect(applied.missing == 0, "missing count") ||
        !expect(applied.extra == 0, "extra count") ||
        !expect(rect_is(root, 0, 0, 400, 300), "root bounds") ||
        !expect(rect_is(panel_ref, 10, 20, 200, 100), "child local bounds") ||
        !expect(rect_is(label_ref, 5, 7, 50, 10), "grandchild local bounds")) {
        return 1;
    }

    // Parent-relative bounds must still compose into absolute world geometry (the contract that
    // damage and paint consume).
    const auto panel_world = panel_ref.world_bounds();
    const auto label_world = label_ref.world_bounds();
    if (!expect(near(panel_world.x, 10.0f) && near(panel_world.y, 20.0f), "panel world origin") ||
        !expect(near(label_world.x, 15.0f) && near(label_world.y, 27.0f),
                "grandchild world origin") ||
        !expect(root.hit_test_deep(20.0f, 30.0f) == &label_ref, "nested hit after layout") ||
        !expect(root.hit_test_deep(20.0f, 20.0f) == &panel_ref, "parent hit after layout")) {
        return 6;
    }

    if (!expect(any_paint_dirty(panel_ref), "child paint dirty") ||
        !expect(any_paint_dirty(label_ref), "grandchild paint dirty") ||
        !expect(tessera::ui::has_any(root.subtree_dirty(), Dirty::Paint | Dirty::Render),
                "root subtree dirty")) {
        return 2;
    }

    root.clear_dirty(Dirty::All);
    panel_ref.clear_dirty(Dirty::All);
    label_ref.clear_dirty(Dirty::All);
    const LayoutApplyResult repeated = tessera::ui::layout::apply_layout(layout, root);
    if (!expect(repeated.applied == 3, "repeat applied") ||
        !expect(root.dirty() == Dirty::None, "same root bounds stay clean") ||
        !expect(panel_ref.dirty() == Dirty::None, "same child bounds stay clean") ||
        !expect(label_ref.dirty() == Dirty::None, "same grandchild bounds stay clean")) {
        return 3;
    }

    const LayoutResult partial{
        {{LayoutBox{1, {0.0f, 0.0f, 400.0f, 300.0f}}, LayoutBox{2, {10.0f, 20.0f, 200.0f, 100.0f}},
          LayoutBox{4, {0.0f, 0.0f, 5.0f, 5.0f}}}}};
    const LayoutApplyResult mismatched = tessera::ui::layout::apply_layout(partial, root);
    if (!expect(mismatched.applied == 2, "partial applied") ||
        !expect(mismatched.missing == 1, "partial missing subtree") ||
        !expect(mismatched.extra == 1, "partial extra box")) {
        return 4;
    }

    const LayoutResult missing_root{{{LayoutBox{2, {10.0f, 20.0f, 200.0f, 100.0f}},
                                      LayoutBox{3, {15.0f, 27.0f, 50.0f, 10.0f}}}}};
    const LayoutApplyResult missing = tessera::ui::layout::apply_layout(missing_root, root);
    if (!expect(missing.applied == 0, "missing root applies nothing") ||
        !expect(missing.missing == 3, "missing root counts tree") ||
        !expect(missing.extra == 2, "missing root extra boxes")) {
        return 5;
    }

    std::cout << "layout_apply: ok\n";
    return 0;
}
