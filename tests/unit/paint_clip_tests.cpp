// SPDX-License-Identifier: MIT
//
// Overflow-clip emission from the paint pass: order relative to a parent's own paint and children,
// per-corner radius clamping, rectangular clips, `overflow: visible`, and nested clips.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/scroll.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_clip: " << message << "\n";
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

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect&, const Color&) override {
        ++fills;
    }
    void fill_rounded_rect(const Rect&, const Color&, float) override {
        ++fills;
    }
    void push_clip(const RoundedClip& clip) override {
        clips.push_back(clip);
        push_after_fills.push_back(fills);
    }
    void pop_clip() override {
        pop_after_fills.push_back(fills);
    }

    std::size_t fills = 0;
    std::vector<RoundedClip> clips;
    std::vector<std::size_t> push_after_fills;
    std::vector<std::size_t> pop_after_fills;
};

bool same_rect(const Rect& actual, const Rect& expected) {
    return actual.x == expected.x && actual.y == expected.y && actual.width == expected.width &&
           actual.height == expected.height;
}

bool test_rounded_clip_emission() {
    Node root(1);
    root.set_bounds({0, 0, 100, 50});
    auto child_owner = std::make_unique<Node>(2);
    child_owner->set_bounds({10, 10, 200, 100});
    root.add_child(std::move(child_owner));

    StubStyles styles;
    styles.set(1, styled({{"background-color", "black"},
                          {"overflow-x", "hidden"},
                          {"overflow-y", "hidden"},
                          {"border-top-left-radius", "4px"},
                          {"border-top-right-radius", "8px"},
                          {"border-bottom-right-radius", "200px"},
                          {"border-bottom-left-radius", "0px"}}));
    styles.set(2, styled({{"background-color", "red"}}));

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);
    return expect(stats.clips == 1, "one rounded overflow clip") &&
           expect(sink.clips.size() == 1, "one recorded clip") &&
           expect(sink.push_after_fills.size() == 1 && sink.push_after_fills[0] == 1,
                  "clip pushes after the parent's background") &&
           expect(sink.pop_after_fills.size() == 1 && sink.pop_after_fills[0] == 2,
                  "clip pops after the children") &&
           expect(same_rect(sink.clips[0].rect, {0, 0, 100, 50}),
                  "clip rect is the parent border box") &&
           expect(sink.clips[0].top_left == 4.0f && sink.clips[0].top_right == 8.0f &&
                      sink.clips[0].bottom_right == 25.0f && sink.clips[0].bottom_left == 0.0f,
                  "per-corner radii clamp to half the smaller side");
}

bool test_visible_overflow_does_not_clip() {
    Node root(1);
    root.set_bounds({0, 0, 40, 40});
    auto child_owner = std::make_unique<Node>(2);
    child_owner->set_bounds({0, 0, 80, 80});
    root.add_child(std::move(child_owner));

    StubStyles styles;
    styles.set(1, styled({{"background-color", "black"},
                          {"overflow-x", "visible"},
                          {"overflow-y", "visible"},
                          {"border-top-left-radius", "8px"}}));
    styles.set(2, styled({{"background-color", "red"}}));

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);
    return expect(stats.clips == 0 && sink.clips.empty(), "visible overflow does not clip") &&
           expect(sink.fills == 2, "children still paint without a clip");
}

bool test_rectangular_clip() {
    Node root(1);
    root.set_bounds({0, 0, 20, 20});
    auto child_owner = std::make_unique<Node>(2);
    child_owner->set_bounds({0, 0, 50, 50});
    root.add_child(std::move(child_owner));

    StubStyles styles;
    styles.set(1, styled({{"overflow-x", "hidden"}, {"overflow-y", "hidden"}}));
    styles.set(2, styled({{"background-color", "red"}}));

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);
    return expect(stats.clips == 1 && sink.clips.size() == 1, "rectangular overflow clips") &&
           expect(!sink.clips[0].rounded(), "rectangular clip has zero radii") &&
           expect(sink.clips[0].top_left == 0.0f && sink.clips[0].bottom_right == 0.0f,
                  "all rectangular radii are zero");
}

bool test_nested_clips() {
    Node root(1);
    root.set_bounds({0, 0, 200, 100});
    auto middle_owner = std::make_unique<Node>(2);
    middle_owner->set_bounds({10, 10, 100, 50});
    Node& middle = root.add_child(std::move(middle_owner));
    auto child_owner = std::make_unique<Node>(3);
    child_owner->set_bounds({0, 0, 300, 300});
    middle.add_child(std::move(child_owner));

    StubStyles styles;
    styles.set(1, styled({{"overflow-x", "hidden"},
                          {"overflow-y", "hidden"},
                          {"border-top-left-radius", "4px"}}));
    styles.set(2, styled({{"overflow-x", "hidden"},
                          {"overflow-y", "hidden"},
                          {"border-top-left-radius", "2px"}}));
    styles.set(3, styled({{"background-color", "red"}}));

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);
    return expect(stats.clips == 2 && sink.clips.size() == 2, "nested overflow clips") &&
           expect(same_rect(sink.clips[0].rect, {0, 0, 200, 100}), "outer clip first") &&
           expect(same_rect(sink.clips[1].rect, {10, 10, 100, 50}), "inner clip second") &&
           expect(sink.pop_after_fills.size() == 2, "both clips pop");
}

// A container's children clip at the padding edge: the border box inset by the painted border
// widths (CSS child clip region), while the container's own background/border still paint over the
// whole border box. Scroll containers always clip; the clip is their scroll viewport boundary.
bool test_padding_box_clip() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    auto container_owner = std::make_unique<Node>(2);
    container_owner->set_bounds({10, 10, 100, 80});
    container_owner->set_scrollable_y(true);
    container_owner->set_content_size(100.0f, 300.0f);
    Node& container = root.add_child(std::move(container_owner));
    auto child_owner = std::make_unique<Node>(3);
    child_owner->set_bounds({0, 150, 20, 20});
    Node& child = container.add_child(std::move(child_owner));

    StubStyles styles;
    styles.set(2, styled({{"border-top-width", "3px"},
                          {"border-top-style", "solid"},
                          {"border-right-width", "5px"},
                          {"border-right-style", "solid"},
                          {"border-bottom-width", "7px"},
                          {"border-bottom-style", "solid"},
                          {"border-left-width", "2px"},
                          {"border-left-style", "solid"}}));
    styles.set(3, styled({{"background-color", "red"}}));

    tessera::ui::scroll_to(container, 0.0f, 120.0f);

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);
    return expect(stats.clips == 1 && sink.clips.size() == 1, "scroll container clips once") &&
           expect(same_rect(sink.clips[0].rect, {12, 13, 93, 70}),
                  "clip is the padding box, not the border box") &&
           expect(!sink.clips[0].rounded(), "rectangular padding-box clip") &&
           expect(child.world_bounds().y == 40.0f, "child keeps its scrolled position");
}

} // namespace

int main() {
    bool ok = true;
    ok = test_rounded_clip_emission() && ok;
    ok = test_visible_overflow_does_not_clip() && ok;
    ok = test_rectangular_clip() && ok;
    ok = test_nested_clips() && ok;
    ok = test_padding_box_clip() && ok;

    if (!ok) {
        return 1;
    }
    std::cout << "paint_clip: ok\n";
    return 0;
}
