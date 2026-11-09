// SPDX-License-Identifier: MIT
//
// Scrollport insets: the clamp viewport is the border box inset by them; 0 (the default) keeps the
// previous border-box behaviour byte-identical; the paint clip and hit-test viewport use the same
// inset box so a bordered container no longer under-reveals at the far edge.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/scroll.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Dirty;
using tessera::ui::has_flag;
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
        std::cerr << "scroll_insets: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::fabs(lhs - rhs) <= 0.001f;
}

bool rect_is(const Rect& rect, float x, float y, float width, float height) {
    return near(rect.x, x) && near(rect.y, y) && near(rect.width, width) &&
           near(rect.height, height);
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
    void fill_rect(const Rect&, const Color&) override {}
    void fill_rounded_rect(const Rect&, const Color&, float) override {}
    void push_clip(const RoundedClip& clip) override {
        clips.push_back(clip);
    }
    void pop_clip() override {}

    std::vector<RoundedClip> clips;
};

bool default_insets_keep_border_box_behaviour() {
    Node node(1);
    node.set_bounds({0, 0, 100, 100});
    node.set_scrollable_y(true);
    node.set_content_size(100.0f, 300.0f);
    return expect(!node.has_scrollport_insets(), "no insets by default") &&
           expect(near(tessera::ui::scrollport_width(node), 100.0f), "border-box extent x") &&
           expect(near(tessera::ui::scrollport_height(node), 100.0f), "border-box extent y") &&
           expect(near(tessera::ui::max_scroll_y(node), 200.0f), "border-box clamp") &&
           expect(tessera::ui::scroll_to(node, 0.0f, 1000.0f), "clamp request changes") &&
           expect(near(node.scroll_y(), 200.0f), "clamped to the border-box maximum");
}

bool insets_shrink_the_scrollport_and_grow_the_clamp() {
    Node node(1);
    node.set_bounds({0, 0, 100, 100});
    node.set_scrollable_y(true);
    node.set_content_size(100.0f, 300.0f);
    node.set_scrollport_insets(6.0f, 10.0f, 4.0f, 20.0f);
    if (!expect(near(node.scrollport_left(), 6.0f) && near(node.scrollport_top(), 10.0f) &&
                    near(node.scrollport_right(), 4.0f) && near(node.scrollport_bottom(), 20.0f),
                "insets stored per side") ||
        !expect(near(tessera::ui::scrollport_width(node), 90.0f), "inset extent x") ||
        !expect(near(tessera::ui::scrollport_height(node), 70.0f), "inset extent y") ||
        !expect(near(tessera::ui::max_scroll_y(node), 230.0f), "inset clamp")) {
        return false;
    }
    return expect(tessera::ui::scroll_to(node, 0.0f, 1000.0f) && near(node.scroll_y(), 230.0f),
                  "clamped to the scrollport maximum");
}

bool insets_sanitize_and_clamp_to_the_box() {
    Node node(1);
    node.set_bounds({0, 0, 100, 100});
    node.set_scrollable_x(true);
    node.set_scrollable_y(true);
    node.set_content_size(300.0f, 300.0f);

    node.set_scrollport_insets(-5.0f, std::numeric_limits<float>::quiet_NaN(), 30.0f, 10.0f);
    if (!expect(near(node.scrollport_left(), 0.0f), "negative inset sanitizes to 0") ||
        !expect(near(node.scrollport_top(), 0.0f), "NaN inset sanitizes to 0") ||
        !expect(near(node.scrollport_right(), 30.0f) && near(node.scrollport_bottom(), 10.0f),
                "positive insets kept") ||
        !expect(near(tessera::ui::scrollport_width(node), 70.0f), "sanitized extent")) {
        return false;
    }

    node.set_scrollport_insets(0.0f, 0.0f, std::numeric_limits<float>::infinity(), 0.0f);
    if (!expect(near(node.scrollport_right(), 0.0f), "infinite inset sanitizes to 0")) {
        return false;
    }

    // Insets that consume the whole box leave a zero viewport; the clamp then covers all content.
    node.set_scrollport_insets(200.0f, 200.0f, 0.0f, 0.0f);
    return expect(near(node.scrollport_left(), 100.0f) && near(node.scrollport_top(), 100.0f),
                  "insets clamp to the box extent") &&
           expect(near(tessera::ui::scrollport_width(node), 0.0f), "zero extent x") &&
           expect(near(tessera::ui::max_scroll_x(node), 300.0f), "zero-viewport clamp");
}

bool insets_reclamp_stored_offsets() {
    Node node(1);
    node.set_bounds({0, 0, 100, 100});
    node.set_scrollable_y(true);
    node.set_content_size(100.0f, 300.0f);
    // A zero-height scrollport makes all content scrollable; restoring it re-clamps the offset.
    node.set_scrollport_insets(0.0f, 50.0f, 0.0f, 50.0f);
    if (!expect(near(tessera::ui::max_scroll_y(node), 300.0f), "zero-height scrollport clamp")) {
        return false;
    }
    if (!expect(tessera::ui::scroll_to(node, 0.0f, 300.0f), "offset reaches the wide clamp")) {
        return false;
    }
    node.set_scrollport_insets(0.0f, 0.0f, 0.0f, 0.0f);
    return expect(near(node.scroll_y(), 200.0f), "offset re-clamped when the viewport grows");
}

bool insets_dirty_paint_and_render_only() {
    Node node(1);
    node.set_bounds({0, 0, 100, 100});
    node.set_scrollable_y(true);
    node.set_content_size(100.0f, 300.0f);
    node.clear_dirty(Dirty::All);
    node.set_scrollport_insets(2.0f, 2.0f, 2.0f, 2.0f);
    if (!expect(has_flag(node.dirty(), Dirty::Paint) && has_flag(node.dirty(), Dirty::Render),
                "insets mark Paint and Render") ||
        !expect(!has_flag(node.dirty(), Dirty::Layout), "insets never mark Layout")) {
        return false;
    }
    node.clear_dirty(Dirty::All);
    node.set_scrollport_insets(2.0f, 2.0f, 2.0f, 2.0f);
    return expect(node.dirty() == Dirty::None, "repeating the same insets marks nothing");
}

bool paint_clip_and_clamp_agree_with_a_border() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    auto container_owner = std::make_unique<Node>(2);
    Node& container = root.add_child(std::move(container_owner));
    container.set_bounds({10, 10, 100, 80});
    container.set_scrollable_y(true);
    container.set_content_size(100.0f, 300.0f);
    container.set_scrollport_insets(2.0f, 3.0f, 5.0f, 7.0f);

    auto child_owner = std::make_unique<Node>(3);
    child_owner->set_bounds({0, 40, 20, 20});
    container.add_child(std::move(child_owner));

    StubStyles styles;
    // The painted border widths match the insets: the clamps and the clip must use the same box.
    styles.set(2, styled({{"border-left-width", "2px"},
                          {"border-left-style", "solid"},
                          {"border-top-width", "3px"},
                          {"border-top-style", "solid"},
                          {"border-right-width", "5px"},
                          {"border-right-style", "solid"},
                          {"border-bottom-width", "7px"},
                          {"border-bottom-style", "solid"}}));

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);
    const Rect clip = sink.clips.empty() ? Rect{} : sink.clips.front().rect;
    return expect(stats.clips == 1 && sink.clips.size() == 1, "one scroll clip emitted") &&
           expect(rect_is(clip, 12, 13, 93, 70), "clip is the inset scrollport") &&
           expect(near(tessera::ui::scrollport_width(container), clip.width),
                  "clamp width equals the painted clip width") &&
           expect(near(tessera::ui::scrollport_height(container), clip.height),
                  "clamp height equals the painted clip height") &&
           expect(near(tessera::ui::max_scroll_y(container), 300.0f - clip.height),
                  "max offset derives from the same height");
}

bool hit_test_clips_to_the_inset_box() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    auto container_owner = std::make_unique<Node>(2);
    Node& container = root.add_child(std::move(container_owner));
    container.set_bounds({0, 0, 100, 100});
    container.set_scrollable_y(true);
    container.set_content_size(100.0f, 300.0f);
    auto child_owner = std::make_unique<Node>(3);
    Node& child = container.add_child(std::move(child_owner));
    child.set_bounds({0, 0, 20, 20});

    if (!expect(root.hit_test_deep(5.0f, 5.0f) == &child,
                "without insets the border box is the hit viewport")) {
        return false;
    }
    container.set_scrollport_insets(10.0f, 10.0f, 10.0f, 10.0f);
    return expect(root.hit_test_deep(5.0f, 5.0f) == &container,
                  "a point in the inset band misses the subtree") &&
           expect(root.hit_test_deep(15.0f, 15.0f) == &child,
                  "a point inside the scrollport still hits the child");
}

} // namespace

int main() {
    if (!default_insets_keep_border_box_behaviour()) {
        return 1;
    }
    if (!insets_shrink_the_scrollport_and_grow_the_clamp()) {
        return 2;
    }
    if (!insets_sanitize_and_clamp_to_the_box()) {
        return 3;
    }
    if (!insets_reclamp_stored_offsets()) {
        return 4;
    }
    if (!insets_dirty_paint_and_render_only()) {
        return 5;
    }
    if (!paint_clip_and_clamp_agree_with_a_border()) {
        return 6;
    }
    if (!hit_test_clips_to_the_inset_box()) {
        return 7;
    }

    std::cout << "scroll_insets: ok\n";
    return 0;
}
