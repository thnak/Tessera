// SPDX-License-Identifier: MIT
//
// Stacking order (CSS z-index): paint order is ascending z-index with child order as the tie-break,
// hit testing is the exact reverse, and the shared walker never reorders equal-z children.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/z_order.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::for_each_by_z;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "z_order: " << message << "\n";
    }
    return condition;
}

class StubStyles final : public StyleProvider {
public:
    void set_color(NodeId id, const char* color) {
        ComputedStyle style;
        style.set("background-color", color);
        styles_[id] = std::move(style);
    }
    ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

class ColorSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color& color) override {
        reds.push_back(color.r);
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        fill_rect(rect, color);
    }
    std::vector<float> reds;
};

// Three overlapping children of one parent; ids 2, 3, 4 in child order.
struct Scene {
    Node root{1};
    Node* a = nullptr;
    Node* b = nullptr;
    Node* c = nullptr;
    StubStyles styles;

    Scene() {
        root.set_bounds({0, 0, 100, 100});
        a = &add(2);
        b = &add(3);
        c = &add(4);
        styles.set_color(2, "#ff0000");
        styles.set_color(3, "#00ff00");
        styles.set_color(4, "#0000ff");
    }
    Node& add(NodeId id) {
        auto child = std::make_unique<Node>(id);
        child->set_bounds({10, 10, 50, 50});
        return root.add_child(std::move(child));
    }
    std::vector<int> painted_channels() {
        ColorSink sink;
        paint_scene(root, styles, sink);
        std::vector<int> channels;
        for (const float red : sink.reds) {
            channels.push_back(red > 0.5f ? 0 : -1); // red child
        }
        return channels;
    }
};

std::vector<NodeId> walk(const Node& parent, bool descending) {
    std::vector<NodeId> ids;
    for_each_by_z(parent.children(), descending, [&](Node& node) {
        ids.push_back(node.id());
        return false;
    });
    return ids;
}

bool equal_z_keeps_child_order() {
    Scene scene;
    return expect(walk(scene.root, false) == std::vector<NodeId>({2, 3, 4}),
                  "equal z-index paints in child order") &&
           expect(walk(scene.root, true) == std::vector<NodeId>({4, 3, 2}),
                  "equal z-index hit-tests in reverse child order") &&
           expect(scene.root.hit_test_deep(20, 20) == scene.c, "the last sibling is on top");
}

bool z_index_reorders_paint_and_hit() {
    Scene scene;
    scene.a->set_z_index(5);  // red child jumps above the others
    scene.b->set_z_index(-2); // green child drops below
    const std::vector<NodeId> paint = walk(scene.root, false);
    const std::vector<NodeId> hit = walk(scene.root, true);
    ColorSink sink;
    paint_scene(scene.root, scene.styles, sink);
    // Paint order: b(-2), c(0), a(5) -> backgrounds draw green, blue, red (root has no background).
    return expect(paint == std::vector<NodeId>({3, 4, 2}), "paint is ascending by z-index") &&
           expect(hit == std::vector<NodeId>({2, 4, 3}), "hit testing is descending by z-index") &&
           expect(scene.root.hit_test_deep(20, 20) == scene.a,
                  "the highest z-index is hit first") &&
           expect(sink.reds.size() == 3 && sink.reds[0] < 0.1f && sink.reds[1] < 0.1f &&
                      sink.reds[2] > 0.9f,
                  "the red (z=5) child paints last, over the others");
}

bool ties_use_child_order_within_a_level() {
    Scene scene;
    scene.a->set_z_index(1);
    scene.c->set_z_index(1);
    return expect(walk(scene.root, false) == std::vector<NodeId>({3, 2, 4}),
                  "equal z levels keep child order, lower levels first") &&
           expect(walk(scene.root, true) == std::vector<NodeId>({4, 2, 3}),
                  "equal z levels hit later siblings first, higher levels first");
}

bool z_index_change_dirties_paint_not_layout() {
    Scene scene;
    scene.root.clear_dirty(tessera::ui::Dirty::Style | tessera::ui::Dirty::Layout |
                           tessera::ui::Dirty::Paint | tessera::ui::Dirty::Render);
    scene.a->clear_dirty(tessera::ui::Dirty::Style | tessera::ui::Dirty::Layout |
                         tessera::ui::Dirty::Paint | tessera::ui::Dirty::Render);
    scene.a->set_z_index(0); // unchanged: no dirt
    const bool clean = !tessera::ui::has_flag(scene.a->dirty(), tessera::ui::Dirty::Paint);
    scene.a->set_z_index(3);
    return expect(clean, "an unchanged z-index dirties nothing") &&
           expect(tessera::ui::has_flag(scene.a->dirty(), tessera::ui::Dirty::Paint) &&
                      !tessera::ui::has_flag(scene.a->dirty(), tessera::ui::Dirty::Layout),
                  "a z-index change dirties paint but not layout");
}

} // namespace

int main() {
    bool ok = true;
    ok = equal_z_keeps_child_order() && ok;
    ok = z_index_reorders_paint_and_hit() && ok;
    ok = ties_use_child_order_within_a_level() && ok;
    ok = z_index_change_dirties_paint_not_layout() && ok;
    if (!ok) {
        std::cerr << "z_order: FAILED\n";
        return 1;
    }
    std::cout << "z_order: ok\n";
    return 0;
}
