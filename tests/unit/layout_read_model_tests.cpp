// SPDX-License-Identifier: MIT
//
// LayoutReadModel (docs/design/layout-observation.md section 5.9): publish/find, generation, a
// node missing from the committed layout, content insets from Yoga padding and border, and the
// visual bounds read from the live node.

#include "tessera/ui/layout/apply_layout.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/layout/layout_read_model.hpp"

#include <cmath>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <utility>

namespace {

using tessera::ui::Node;
using tessera::ui::layout::LayoutEngine;
using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutReadModel;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layout_read_model: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.01f;
}

ComputedStyle style(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle result;
    for (const auto& [property, value] : values) {
        result.set(property, value);
    }
    return result;
}

LayoutInput node(std::uint64_t id, ComputedStyle computed, std::vector<LayoutInput> children = {}) {
    return LayoutInput{id, std::move(computed), std::move(children), {}};
}

bool insets_from_padding_and_border() {
    LayoutEngine engine;
    // 200x100 border-box panel with 10px padding and a 2px border on every side.
    const LayoutInput panel = node(2,
                                   style({{"display", "flex"},
                                          {"box-sizing", "border-box"},
                                          {"width", "200px"},
                                          {"height", "100px"},
                                          {"padding-top", "10px"},
                                          {"padding-right", "10px"},
                                          {"padding-bottom", "10px"},
                                          {"padding-left", "10px"},
                                          {"border-top-width", "2px"},
                                          {"border-right-width", "2px"},
                                          {"border-bottom-width", "2px"},
                                          {"border-left-width", "2px"},
                                          {"border-top-style", "solid"},
                                          {"border-right-style", "solid"},
                                          {"border-bottom-style", "solid"},
                                          {"border-left-style", "solid"}}),
                                   {node(3, style({{"width", "50px"}, {"height", "20px"}}))});
    const LayoutInput root =
        node(1, style({{"display", "flex"}, {"width", "400px"}, {"height", "300px"}}), {panel});
    const LayoutResult result = engine.layout(root, LayoutViewport{400.0f, 300.0f});

    const tessera::ui::layout::LayoutBox* panel_box = nullptr;
    const tessera::ui::layout::LayoutBox* root_box = nullptr;
    for (const auto& box : result.boxes) {
        if (box.id == 2) {
            panel_box = &box;
        }
        if (box.id == 1) {
            root_box = &box;
        }
    }
    if (!expect(panel_box != nullptr && root_box != nullptr, "both boxes exist") ||
        !expect(near(panel_box->content_insets.left, 12.0f) &&
                    near(panel_box->content_insets.top, 12.0f) &&
                    near(panel_box->content_insets.right, 12.0f) &&
                    near(panel_box->content_insets.bottom, 12.0f),
                "padding plus border make a 12px inset on every side") ||
        !expect(root_box->content_insets == tessera::ui::layout::LayoutInsets{},
                "an unpadded box has zero insets")) {
        return false;
    }

    Node scene_root(1);
    Node& scene_panel = scene_root.add_child(std::make_unique<Node>(2));
    scene_panel.add_child(std::make_unique<Node>(3));
    tessera::ui::layout::apply_layout(result, scene_root);

    LayoutReadModel model;
    model.publish(result, 2.0f);
    const auto info = model.find(scene_panel);
    if (!expect(info.has_value(), "the panel is in the read model") ||
        !expect(near(info->border_box.width, 200.0f) && near(info->border_box.height, 100.0f),
                "border box is the Yoga box") ||
        !expect(near(info->content_box.x, info->border_box.x + 12.0f) &&
                    near(info->content_box.y, info->border_box.y + 12.0f) &&
                    near(info->content_box.width, 176.0f) && near(info->content_box.height, 76.0f),
                "content box is the border box inset by padding and border") ||
        !expect(near(info->dpi_scale, 2.0f), "dpi scale is the published one") ||
        !expect(info->generation == 1, "first publish is generation 1")) {
        return false;
    }
    return true;
}

bool generation_and_missing_nodes() {
    LayoutEngine engine;
    const LayoutInput root =
        node(1, style({{"display", "flex"}, {"width", "100px"}, {"height", "50px"}}),
             {node(2, style({{"width", "10px"}, {"height", "10px"}}))});
    const LayoutResult result = engine.layout(root, LayoutViewport{100.0f, 50.0f});

    Node scene_root(1);
    Node& child = scene_root.add_child(std::make_unique<Node>(2));
    Node late(99);

    LayoutReadModel model;
    if (!expect(model.generation() == 0 && model.size() == 0, "an empty model is generation 0") ||
        !expect(!model.find(child).has_value(), "nothing is known before the first publish")) {
        return false;
    }
    model.publish(result, 1.0f);
    if (!expect(model.generation() == 1 && model.size() == 2, "publish stores every box") ||
        !expect(model.find(child).has_value(), "a laid out node is found") ||
        !expect(!model.find(late).has_value(), "a node added since the layout is not found")) {
        return false;
    }
    model.publish(result, 1.0f);
    if (!expect(model.generation() == 2, "every publish bumps the generation") ||
        !expect(model.find(child)->generation == 2, "a snapshot carries its publish generation")) {
        return false;
    }

    // A smaller tree replaces the old entries instead of accumulating them.
    const LayoutResult single =
        engine.layout(node(1, style({{"display", "flex"}, {"width", "10px"}, {"height", "10px"}})),
                      LayoutViewport{10.0f, 10.0f});
    model.publish(single, 1.0f);
    return expect(model.size() == 1 && !model.find(child).has_value(),
                  "a later publish drops nodes that left the layout");
}

bool visual_bounds_follow_the_node() {
    LayoutEngine engine;
    const LayoutInput root =
        node(1, style({{"display", "flex"}, {"width", "100px"}, {"height", "50px"}}),
             {node(2, style({{"width", "10px"}, {"height", "10px"}}))});
    const LayoutResult result = engine.layout(root, LayoutViewport{100.0f, 50.0f});
    Node scene_root(1);
    Node& child = scene_root.add_child(std::make_unique<Node>(2));
    tessera::ui::layout::apply_layout(result, scene_root);

    LayoutReadModel model;
    model.publish(result, 1.0f);
    const auto before = model.find(child);
    child.set_bounds({30.0f, 5.0f, 10.0f, 10.0f}); // moved after publish: visual bounds are live
    const auto after = model.find(child);
    return expect(before.has_value() && after.has_value(), "both reads succeed") &&
           expect(near(after->border_box.x, before->border_box.x),
                  "the committed border box does not change until the next publish") &&
           expect(near(after->visual_bounds.x, child.world_bounds().x),
                  "visual bounds are read from the node at call time");
}

} // namespace

int main() {
    if (!insets_from_padding_and_border()) {
        return 1;
    }
    if (!generation_and_missing_nodes()) {
        return 2;
    }
    if (!visual_bounds_follow_the_node()) {
        return 3;
    }
    std::cout << "layout_read_model: ok\n";
    return 0;
}
