// SPDX-License-Identifier: MIT
//
// Style -> scene sync: z-index, transform and box-shadow reach are copied from computed style onto
// nodes, only dirty what changed, skip invisible subtrees, and drive hit testing and damage.

#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <utility>

namespace {

using tessera::ui::Dirty;
using tessera::ui::has_flag;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::sync_scene_style;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "scene_style_sync: " << message << "\n";
    }
    return condition;
}

ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

bool near(float a, float b) {
    return std::abs(a - b) < 0.01f;
}

constexpr Dirty kVisual = Dirty::Paint | Dirty::Render;

bool z_index_values() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    Node& a = root.add_child(std::make_unique<Node>(2));
    Node& b = root.add_child(std::make_unique<Node>(3));
    Node& c = root.add_child(std::make_unique<Node>(4));
    Node& d = root.add_child(std::make_unique<Node>(5));
    StubStyles styles;
    styles.set(2, styled({{"z-index", "5"}}));
    styles.set(3, styled({{"z-index", "auto"}}));
    styles.set(4, styled({{"z-index", "-2"}}));
    styles.set(5, styled({{"z-index", "abc"}}));
    const auto result = sync_scene_style(root, styles);
    return expect(a.z_index() == 5 && b.z_index() == 0 && c.z_index() == -2 && d.z_index() == 0,
                  "z-index integers apply; auto and invalid values are 0") &&
           expect(result.nodes == 5 && result.changed_z_index == 2, "two nodes changed");
}

bool transform_drives_bounds_and_hit_testing() {
    Node root(1);
    root.set_bounds({0, 0, 400, 400});
    Node& box = root.add_child(std::make_unique<Node>(2));
    box.set_bounds({10, 10, 100, 50});
    Node& child = box.add_child(std::make_unique<Node>(3));
    child.set_bounds({0, 0, 20, 20});
    StubStyles styles;
    styles.set(2, styled({{"transform", "translate(20px, 0)"}}));
    sync_scene_style(root, styles);
    bool ok = expect(near(box.world_bounds().x, 30) && near(box.world_bounds().width, 100),
                     "translate moves the box") &&
              expect(box.hit_test(35, 20) && !box.hit_test(15, 20),
                     "hit testing follows the transform") &&
              expect(near(child.world_bounds().x, 30), "children move with their parent");

    styles.set(2, styled({{"transform", "scale(2)"}}));
    sync_scene_style(root, styles);
    // Scale about the center (60, 35): the 100x50 box becomes 200x100 starting at (-40, -15).
    ok = expect(near(box.world_bounds().x, -40) && near(box.world_bounds().y, -15) &&
                    near(box.world_bounds().width, 200) && near(box.world_bounds().height, 100),
                "scale grows the box about the center") &&
         expect(near(child.world_bounds().width, 40), "children scale with their parent") && ok;

    styles.set(2, styled({{"transform", "scale(2)"}, {"transform-origin", "left top"}}));
    sync_scene_style(root, styles);
    ok = expect(near(box.world_bounds().x, 10) && near(box.world_bounds().y, 10) &&
                    near(box.world_bounds().width, 200),
                "transform-origin anchors the scale at the corner") &&
         ok;

    styles.set(2, styled({{"transform", "rotate(90deg)"}}));
    sync_scene_style(root, styles);
    // A quarter turn about the center swaps width and height (100x50 -> 50x100) around (60, 35).
    ok = expect(near(box.world_bounds().width, 50) && near(box.world_bounds().height, 100),
                "a 90 degree rotation swaps the box extents") &&
         ok;

    styles.set(2, styled({{"transform", "bogus(1)"}}));
    sync_scene_style(root, styles);
    return expect(near(box.world_bounds().width, 100) && near(box.world_bounds().x, 10),
                  "an invalid transform leaves the node untransformed") &&
           ok;
}

bool shadow_outsets_reach_damage() {
    Node root(1);
    root.set_bounds({0, 0, 400, 400});
    Node& box = root.add_child(std::make_unique<Node>(2));
    box.set_bounds({50, 50, 100, 40});
    StubStyles styles;
    styles.set(2, styled({{"box-shadow", "6px 4px 10px 2px #000000"}}));
    const auto result = sync_scene_style(root, styles);
    const auto damage = tessera::ui::layout::node_damage_rect(box);
    // reach = spread 2 + blur 10 = 12: left 12-6, right 12+6, top 12-4, bottom 12+4.
    return expect(result.changed_outsets == 1, "the node picked up shadow outsets") &&
           expect(near(box.paint_outsets().left, 6) && near(box.paint_outsets().right, 18) &&
                      near(box.paint_outsets().top, 8) && near(box.paint_outsets().bottom, 16),
                  "outsets match offset, spread and blur") &&
           expect(near(damage.x, 44) && near(damage.y, 42) && near(damage.width, 124) &&
                      near(damage.height, 64),
                  "damage covers the shadow") &&
           expect(near(box.world_bounds().x, 50), "the border box itself is unchanged");
}

bool only_changes_dirty_and_invisible_skipped() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    Node& box = root.add_child(std::make_unique<Node>(2));
    box.set_bounds({0, 0, 20, 20});
    Node& hidden = root.add_child(std::make_unique<Node>(3));
    hidden.set_bounds({0, 0, 20, 20});
    hidden.set_visible(false);
    StubStyles styles;
    styles.set(2, styled({{"z-index", "3"},
                          {"transform", "translate(5px, 5px)"},
                          {"box-shadow", "0 0 4px #000"}}));
    styles.set(3, styled({{"z-index", "9"}}));
    sync_scene_style(root, styles);
    box.clear_dirty(Dirty::Style | Dirty::Layout | kVisual);
    const auto again = sync_scene_style(root, styles);
    return expect(again.changed_z_index == 0 && again.changed_transform == 0 &&
                      again.changed_outsets == 0,
                  "a second sync changes nothing") &&
           expect(!has_flag(box.dirty(), kVisual), "an unchanged scene stays clean") &&
           expect(hidden.z_index() == 0, "invisible subtrees are skipped") &&
           expect(again.nodes == 2, "only visible nodes are resolved");
}

} // namespace

int main() {
    bool ok = true;
    ok = z_index_values() && ok;
    ok = transform_drives_bounds_and_hit_testing() && ok;
    ok = shadow_outsets_reach_damage() && ok;
    ok = only_changes_dirty_and_invisible_skipped() && ok;
    if (!ok) {
        std::cerr << "scene_style_sync: FAILED\n";
        return 1;
    }
    std::cout << "scene_style_sync: ok\n";
    return 0;
}
