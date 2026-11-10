// SPDX-License-Identifier: MIT
//
// CSS transform painting: a sink that supports matrices receives each transformed node in its own
// local frame (push_transform / local rects / pop_transform), nested transforms compose, scroll and
// overflow clips map through the frame, and sinks without matrix support keep the AABB fallback.

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/transform.hpp"

#include <cmath>
#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Mat3;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "paint_transform: " << message << "\n";
    }
    return condition;
}

bool near(float a, float b) {
    return std::abs(a - b) < 0.01f;
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

ComputedStyle background(const char* color) {
    ComputedStyle style;
    style.set("background-color", color);
    return style;
}

struct Event {
    std::string kind; // "fill", "push", "pop", "clip", "unclip"
    Rect rect;
    Mat3 matrix;
};

class RecordingSink final : public PaintSink {
public:
    explicit RecordingSink(bool matrices)
        : matrices_(matrices) {}
    bool supports_transform() const override {
        return matrices_;
    }
    void push_transform(const Mat3& matrix) override {
        events.push_back({"push", {}, matrix});
    }
    void pop_transform() override {
        events.push_back({"pop", {}, {}});
    }
    void fill_rect(const Rect& rect, const tessera::rendering::rhi::Color&) override {
        events.push_back({"fill", rect, {}});
    }
    void fill_rounded_rect(const Rect& rect, const tessera::rendering::rhi::Color&,
                           float) override {
        events.push_back({"fill", rect, {}});
    }
    void push_clip(const RoundedClip& clip) override {
        events.push_back({"clip", clip.rect, {}});
    }
    void pop_clip() override {
        events.push_back({"unclip", {}, {}});
    }
    std::vector<Event> events;

private:
    bool matrices_;
};

void transform(Node& node, const char* value) {
    const auto matrix = tessera::ui::style::compute_transform(value, "50% 50%", node.bounds().width,
                                                              node.bounds().height);
    node.set_transform(matrix.value_or(Mat3::identity()));
}

bool rotated_node_paints_in_its_local_frame() {
    Node root(1);
    root.set_bounds({0, 0, 400, 400});
    Node& box = root.add_child(std::make_unique<Node>(2));
    box.set_bounds({100, 50, 80, 40});
    transform(box, "rotate(45deg)");
    StubStyles styles;
    styles.set(2, background("#ff0000"));

    RecordingSink with(true);
    const auto stats = paint_scene(root, styles, with);
    RecordingSink without(false);
    paint_scene(root, styles, without);

    bool ok = expect(stats.transforms == 1, "one node opened a transformed frame");
    ok = expect(with.events.size() == 3 && with.events[0].kind == "push" &&
                    with.events[1].kind == "fill" && with.events[2].kind == "pop",
                "push, fill, pop in order") &&
         ok;
    if (with.events.size() == 3) {
        // The box fills its own local rect; the pushed matrix is the node's full world matrix.
        ok = expect(near(with.events[1].rect.x, 0) && near(with.events[1].rect.width, 80) &&
                        near(with.events[1].rect.height, 40),
                    "the fill is the local box") &&
             ok;
        float x = 0;
        float y = 0;
        with.events[0].matrix.transform_point(40, 20, x, y);
        ok = expect(near(x, 140) && near(y, 70), "the box center stays put under rotation") && ok;
    }
    return expect(without.events.size() == 1 && without.events[0].kind == "fill",
                  "a sink without matrix support gets one AABB fill and no transform calls") &&
           expect(without.events.size() == 1 && without.events[0].rect.width > 80.0f,
                  "the fallback rect is the rotated bounding box") &&
           ok;
}

bool nested_transforms_compose_and_children_stay_local() {
    Node root(1);
    root.set_bounds({0, 0, 400, 400});
    Node& outer = root.add_child(std::make_unique<Node>(2));
    outer.set_bounds({100, 100, 100, 100});
    transform(outer, "scale(2)");
    Node& inner = outer.add_child(std::make_unique<Node>(3));
    inner.set_bounds({10, 20, 30, 30});
    StubStyles styles;
    styles.set(2, background("#ff0000"));
    styles.set(3, background("#00ff00"));

    RecordingSink sink(true);
    paint_scene(root, styles, sink);
    // outer: push(world), fill local box; inner has no transform: drawn at its offset inside
    // outer's frame (the sink matrix scales it), not at world coordinates.
    bool ok = expect(sink.events.size() == 4, "push, fill, fill, pop");
    if (sink.events.size() == 4) {
        ok = expect(near(sink.events[2].rect.x, 10) && near(sink.events[2].rect.y, 20) &&
                        near(sink.events[2].rect.width, 30),
                    "a child of a transformed node is painted relative to it") &&
             ok;
    }
    return ok;
}

bool nested_transform_pushes_the_relative_matrix() {
    Node root(1);
    root.set_bounds({0, 0, 400, 400});
    Node& outer = root.add_child(std::make_unique<Node>(2));
    outer.set_bounds({0, 0, 100, 100});
    transform(outer, "translate(50px, 0)");
    Node& inner = outer.add_child(std::make_unique<Node>(3));
    inner.set_bounds({10, 10, 20, 20});
    transform(inner, "translate(0, 7px)");
    StubStyles styles;
    styles.set(3, background("#00ff00"));

    RecordingSink sink(true);
    paint_scene(root, styles, sink);
    // Pushed matrices compose on the sink: the inner push is only inner's own offset + transform.
    bool ok = expect(sink.events.size() == 5 && sink.events[0].kind == "push" &&
                         sink.events[1].kind == "push" && sink.events[2].kind == "fill" &&
                         sink.events[3].kind == "pop" && sink.events[4].kind == "pop",
                     "two pushes, a fill, two pops");
    if (sink.events.size() >= 2) {
        const Mat3& inner_push = sink.events[1].matrix;
        ok = expect(near(inner_push.m[2], 10) && near(inner_push.m[5], 17),
                    "the inner push is relative to the outer frame") &&
             ok;
    }
    return ok;
}

bool clips_map_through_the_frame() {
    Node root(1);
    root.set_bounds({0, 0, 400, 400});
    Node& box = root.add_child(std::make_unique<Node>(2));
    box.set_bounds({100, 100, 50, 50});
    transform(box, "scale(2)");
    Node& rotated = root.add_child(std::make_unique<Node>(3));
    rotated.set_bounds({200, 100, 50, 50});
    transform(rotated, "rotate(30deg)");
    StubStyles styles;
    ComputedStyle clipping;
    clipping.set("overflow-x", "hidden");
    clipping.set("overflow-y", "hidden");
    styles.set(2, clipping);
    styles.set(3, clipping);

    RecordingSink sink(true);
    paint_scene(root, styles, sink);
    bool saw_scaled_clip = false;
    int clips = 0;
    for (const Event& event : sink.events) {
        if (event.kind == "clip") {
            ++clips;
            // Scale 2 about the center (125,125): the 50x50 clip becomes 100x100 at (75,75).
            saw_scaled_clip = near(event.rect.x, 75) && near(event.rect.width, 100);
        }
    }
    return expect(clips == 1 && saw_scaled_clip,
                  "a scaled frame maps its overflow clip to device space; a rotated one drops it");
}

} // namespace

int main() {
    bool ok = true;
    ok = rotated_node_paints_in_its_local_frame() && ok;
    ok = nested_transforms_compose_and_children_stay_local() && ok;
    ok = nested_transform_pushes_the_relative_matrix() && ok;
    ok = clips_map_through_the_frame() && ok;
    if (!ok) {
        std::cerr << "paint_transform: FAILED\n";
        return 1;
    }
    std::cout << "paint_transform: ok\n";
    return 0;
}
