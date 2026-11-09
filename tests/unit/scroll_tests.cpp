// SPDX-License-Identifier: MIT

#include "tessera/ui/input.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/dirty_pass.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/scroll.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::has_flag;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
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
        std::cerr << "scroll: " << message << "\n";
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

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

InputEvent wheel(float x, float y, float dx, float dy) {
    InputEvent event;
    event.type = InputEventType::Wheel;
    event.x = x;
    event.y = y;
    event.dx = dx;
    event.dy = dy;
    return event;
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

struct RecordedOp {
    enum class Kind { Rect, PushClip, PopClip };

    Kind kind = Kind::Rect;
    Rect rect;
    RoundedClip clip;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color&) override {
        ops.push_back({RecordedOp::Kind::Rect, rect, {}});
    }

    void fill_rounded_rect(const Rect& rect, const Color&, float) override {
        ops.push_back({RecordedOp::Kind::Rect, rect, {}});
    }

    void push_clip(const RoundedClip& clip) override {
        ops.push_back({RecordedOp::Kind::PushClip, clip.rect, clip});
        ++depth;
        max_depth = depth > max_depth ? depth : max_depth;
    }

    void pop_clip() override {
        ops.push_back({RecordedOp::Kind::PopClip, {}, {}});
        --depth;
    }

    std::vector<RecordedOp> ops;
    int depth = 0;
    int max_depth = 0;
};

// A scrollable container with an inner hit target: bounds {0,0,100,100}, content 100x300.
struct ScrollScene {
    ScrollScene() {
        root.set_bounds({0, 0, 200, 200});
        auto container_ptr = std::make_unique<Node>(2);
        container = container_ptr.get();
        container->set_bounds({10, 10, 100, 100});
        container->set_scrollable_y(true);
        container->set_content_size(100.0f, 300.0f);
        root.add_child(std::move(container_ptr));

        auto child_ptr = std::make_unique<Node>(3);
        child = child_ptr.get();
        child->set_bounds({20, 150, 20, 20});
        container->add_child(std::move(child_ptr));
    }

    Node root{1};
    Node* container = nullptr;
    Node* child = nullptr;
};

bool clamping_and_dirty() {
    Node root(1);
    root.set_bounds({0, 0, 100, 80});
    root.set_scrollable_x(true);
    root.set_scrollable_y(true);
    root.set_content_size(200.0f, 100.0f);
    clear_tree(root);

    if (!expect(near(tessera::ui::max_scroll_x(root), 100.0f), "max x from content") ||
        !expect(near(tessera::ui::max_scroll_y(root), 20.0f), "max y from content") ||
        !expect(tessera::ui::scroll_to(root, 500.0f, 500.0f), "over-large offset changes") ||
        !expect(near(root.scroll_x(), 100.0f) && near(root.scroll_y(), 20.0f), "clamped to max") ||
        !expect(tessera::ui::scroll_to(root, -10.0f, -10.0f), "negative offset changes") ||
        !expect(near(root.scroll_x(), 0.0f) && near(root.scroll_y(), 0.0f), "clamped to zero") ||
        !expect(tessera::ui::scroll_by(root, 30.0f, 10.0f), "scroll_by applies deltas") ||
        !expect(near(root.scroll_x(), 30.0f) && near(root.scroll_y(), 10.0f), "offset updated") ||
        !expect(!tessera::ui::scroll_to(root, 30.0f, 10.0f), "no change reports false")) {
        return false;
    }

    if (!expect(has_flag(root.dirty(), Dirty::Paint) && has_flag(root.dirty(), Dirty::Render),
                "scroll marks Paint and Render") ||
        !expect(!has_flag(root.dirty(), Dirty::Layout), "scroll never marks Layout")) {
        return false;
    }

    Node plain(2);
    plain.set_bounds({0, 0, 100, 100});
    plain.set_content_size(500.0f, 500.0f);
    clear_tree(plain);
    if (!expect(!tessera::ui::scroll_to(plain, 50.0f, 50.0f), "non-scrollable ignores requests") ||
        !expect(near(plain.scroll_x(), 0.0f) && near(plain.scroll_y(), 0.0f),
                "non-scrollable stays") ||
        !expect(plain.dirty() == Dirty::None, "non-scrollable marks nothing")) {
        return false;
    }

    Node small(3);
    small.set_bounds({0, 0, 100, 100});
    small.set_scrollable_x(true);
    small.set_scrollable_y(true);
    small.set_content_size(50.0f, 50.0f);
    clear_tree(small);
    if (!expect(!tessera::ui::scroll_to(small, 10.0f, 10.0f), "small content clamps to zero") ||
        !expect(small.dirty() == Dirty::None, "clamped-to-zero marks nothing")) {
        return false;
    }

    // A viewport resize re-clamps the stored offset instead of leaving it out of range.
    root.set_bounds({0, 0, 200, 200});
    return expect(near(root.scroll_x(), 0.0f) && near(root.scroll_y(), 0.0f),
                  "viewport growth re-clamps the offset");
}

bool hit_test_with_offset() {
    ScrollScene scene;
    if (!expect(scene.root.hit_test_deep(35.0f, 65.0f) == scene.container,
                "scrolled-out child is clipped out")) {
        return false;
    }
    if (!expect(tessera::ui::scroll_to(*scene.container, 0.0f, 100.0f), "scroll container") ||
        !expect(scene.root.hit_test_deep(35.0f, 65.0f) == scene.child,
                "hit accounts for the scroll offset") ||
        !expect(scene.root.hit_test_deep(35.0f, 35.0f) == scene.container,
                "clipped child misses outside its shifted box") ||
        !expect(scene.child->world_bounds().y == 60.0f, "child world rect is offset")) {
        return false;
    }

    // Points outside the container box never hit the subtree.
    return expect(scene.root.hit_test_deep(35.0f, 165.0f) == &scene.root,
                  "outside the container box misses the subtree");
}

bool wheel_routing() {
    ScrollScene scene;
    InputRouter router;
    const Rect container_before = scene.container->world_bounds();

    const auto result = router.dispatch(scene.root, wheel(35.0f, 35.0f, 0.0f, 1.0f));
    if (!expect(result.dispatched, "wheel dispatched") ||
        !expect(result.target == scene.container, "wheel target is the deepest node") ||
        !expect(near(scene.container->scroll_y(), tessera::ui::kPixelsPerWheelUnit),
                "wheel down scrolls content down by one unit") ||
        !expect(rect_is(scene.container->world_bounds(), container_before.x, container_before.y,
                        container_before.width, container_before.height),
                "container box does not move")) {
        return false;
    }

    if (!expect(tessera::ui::scroll_to(*scene.container, 0.0f, 0.0f), "reset offset")) {
        return false;
    }
    const auto up_wheel = router.dispatch(scene.root, wheel(35.0f, 35.0f, 0.0f, -1.0f));
    if (!expect(up_wheel.dispatched, "up wheel dispatches") ||
        !expect(near(scene.container->scroll_y(), 0.0f), "up wheel clamps at the top")) {
        return false;
    }

    // Non-scrollable chain: dispatch happens, nothing scrolls.
    if (!expect(router.dispatch(scene.root, wheel(150.0f, 150.0f, 0.0f, 1.0f)).dispatched,
                "wheel over the plain root dispatches")) {
        return false;
    }
    return expect(near(scene.container->scroll_y(), 0.0f), "non-scrollable chain does not scroll");
}

class ConsumingNode : public Node {
public:
    using Node::Node;

    void on_event(tessera::ui::Event& event) override {
        if (consume && event.type == tessera::ui::EventType::Wheel) {
            event.prevent_default = true;
        }
    }

    bool consume = false;
};

bool wheel_prevent_default() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    auto container_ptr = std::make_unique<ConsumingNode>(2);
    auto* container = static_cast<ConsumingNode*>(container_ptr.get());
    container->set_bounds({0, 0, 100, 100});
    container->set_scrollable_y(true);
    container->set_content_size(100.0f, 300.0f);
    root.add_child(std::move(container_ptr));

    InputRouter router;
    container->consume = true;
    const auto result = router.dispatch(root, wheel(50.0f, 50.0f, 0.0f, 1.0f));
    return expect(result.dispatched, "consumed wheel still dispatches") &&
           expect(near(container->scroll_y(), 0.0f), "prevent_default suppresses the scroll");
}

bool nested_containers() {
    Node root(1);
    root.set_bounds({0, 0, 300, 300});
    auto outer_ptr = std::make_unique<Node>(2);
    outer_ptr->set_bounds({0, 0, 200, 200});
    outer_ptr->set_scrollable_y(true);
    outer_ptr->set_content_size(200.0f, 500.0f);
    Node& outer = root.add_child(std::move(outer_ptr));
    auto inner_ptr = std::make_unique<Node>(3);
    inner_ptr->set_bounds({10, 10, 100, 100});
    inner_ptr->set_scrollable_y(true);
    inner_ptr->set_content_size(100.0f, 300.0f);
    Node& inner = outer.add_child(std::move(inner_ptr));
    auto child_ptr = std::make_unique<Node>(4);
    child_ptr->set_bounds({20, 20, 20, 20});
    inner.add_child(std::move(child_ptr));

    InputRouter router;
    if (!expect(router.dispatch(root, wheel(35.0f, 35.0f, 0.0f, 1.0f)).dispatched,
                "nested wheel dispatches") ||
        !expect(near(inner.scroll_y(), tessera::ui::kPixelsPerWheelUnit),
                "inner container consumes the wheel") ||
        !expect(near(outer.scroll_y(), 0.0f), "outer container is untouched (no chaining)")) {
        return false;
    }
    return true;
}

bool paint_offset_and_clip() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    auto container_ptr = std::make_unique<Node>(2);
    container_ptr->set_bounds({0, 0, 100, 100});
    container_ptr->set_scrollable_y(true);
    container_ptr->set_content_size(100.0f, 300.0f);
    Node& container = root.add_child(std::move(container_ptr));
    auto child_ptr = std::make_unique<Node>(3);
    child_ptr->set_bounds({0, 40, 20, 20});
    container.add_child(std::move(child_ptr));

    tessera::ui::scroll_to(container, 0.0f, 40.0f);

    StubStyles styles;
    ComputedStyle container_style;
    container_style.set("background-color", "red");
    styles.set(2, container_style);
    ComputedStyle child_style;
    child_style.set("background-color", "blue");
    styles.set(3, child_style);

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink);
    if (!expect(stats.clips == 1, "scroll container pushes a clip") ||
        !expect(sink.ops.size() == 4, "background, clip, child, pop") ||
        !expect(sink.ops[0].kind == RecordedOp::Kind::Rect &&
                    rect_is(sink.ops[0].rect, 0, 0, 100, 100),
                "container background painted at its own box") ||
        !expect(sink.ops[1].kind == RecordedOp::Kind::PushClip &&
                    rect_is(sink.ops[1].rect, 0, 0, 100, 100),
                "clip is the container border box") ||
        !expect(sink.ops[2].kind == RecordedOp::Kind::Rect &&
                    rect_is(sink.ops[2].rect, 0, 0, 20, 20),
                "child is offset by -scroll") ||
        !expect(sink.ops[3].kind == RecordedOp::Kind::PopClip, "clip popped after children") ||
        !expect(sink.depth == 0 && sink.max_depth == 1, "balanced clip nesting")) {
        return false;
    }

    // Painting twice is deterministic and emits the same commands.
    RecordingSink again;
    paint_scene(root, styles, again);
    if (!expect(again.ops.size() == sink.ops.size(), "repeat paint emits the same op count")) {
        return false;
    }
    for (std::size_t i = 0; i < again.ops.size(); ++i) {
        if (!expect(again.ops[i].kind == sink.ops[i].kind, "repeat paint op kinds")) {
            return false;
        }
        if (!expect(rect_is(again.ops[i].rect, sink.ops[i].rect.x, sink.ops[i].rect.y,
                            sink.ops[i].rect.width, sink.ops[i].rect.height),
                    "repeat paint rects")) {
            return false;
        }
    }
    return true;
}

bool damage_after_scroll() {
    ScrollScene scene;
    clear_tree(scene.root);
    if (!expect(tessera::ui::scroll_to(*scene.container, 0.0f, 50.0f), "scroll changes offset")) {
        return false;
    }
    if (!expect(has_flag(scene.container->dirty(), Dirty::Paint) &&
                    has_flag(scene.container->dirty(), Dirty::Render),
                "scroll marks the container dirty") ||
        !expect(!has_flag(scene.container->dirty(), Dirty::Layout), "scroll leaves layout clean")) {
        return false;
    }

    DirtyPass dirty;
    dirty.collect(scene.root, Dirty::Paint | Dirty::Render);
    DamageAccumulator damage(DamageConfig{Extent2D{400, 400}, 4});
    tessera::ui::layout::add_scene_damage(dirty, damage, 1.0f);
    return expect(damage.rects().size() == 1, "one damage rect for the container") &&
           expect(rect_is(damage.rects()[0], 10, 10, 100, 100),
                  "damage covers the container (old and new visible content)");
}

bool scroll_into_view_and_determinism() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    root.set_scrollable_x(true);
    root.set_scrollable_y(true);
    root.set_content_size(300.0f, 300.0f);
    auto child_ptr = std::make_unique<Node>(2);
    child_ptr->set_bounds({150, 150, 20, 20});
    Node& child = root.add_child(std::move(child_ptr));

    if (!expect(tessera::ui::scroll_into_view(child), "scroll_into_view moves the container") ||
        !expect(near(root.scroll_x(), 70.0f) && near(root.scroll_y(), 70.0f),
                "minimal offset brings the child fully into view") ||
        !expect(!tessera::ui::scroll_into_view(child), "already visible child does not scroll")) {
        return false;
    }

    child.set_bounds({10, 10, 20, 20});
    if (!expect(tessera::ui::scroll_into_view(child), "scroll back up") ||
        !expect(near(root.scroll_x(), 10.0f) && near(root.scroll_y(), 10.0f),
                "scroll clamps to the child's top-left")) {
        return false;
    }

    // Deterministic replay: the same request sequence yields the same offsets.
    Node replay(10);
    replay.set_bounds({0, 0, 100, 100});
    replay.set_scrollable_x(true);
    replay.set_scrollable_y(true);
    replay.set_content_size(300.0f, 300.0f);
    tessera::ui::scroll_by(replay, 12.5f, 7.5f);
    tessera::ui::scroll_to(replay, 500.0f, -3.0f);
    return expect(near(replay.scroll_x(), 200.0f) && near(replay.scroll_y(), 0.0f),
                  "replay clamps deterministically");
}

bool scrollable_flag_dirty() {
    Node node(1);
    node.set_bounds({0, 0, 100, 100});
    node.set_content_size(100.0f, 300.0f);
    clear_tree(node);

    if (!expect(!has_flag(node.dirty(), Dirty::Paint), "clean before the flag change")) {
        return false;
    }
    node.set_scrollable_y(true);
    if (!expect(has_flag(node.dirty(), Dirty::Paint) && has_flag(node.dirty(), Dirty::Render),
                "becoming scrollable marks Paint and Render") ||
        !expect(!has_flag(node.dirty(), Dirty::Layout), "becoming scrollable never marks Layout") ||
        !expect(near(tessera::ui::max_scroll_y(node), 200.0f),
                "the scroll limit applies after the flag change")) {
        return false;
    }

    // Repeating the same value is a no-op and marks nothing.
    clear_tree(node);
    node.set_scrollable_y(true);
    return expect(node.dirty() == Dirty::None, "repeated flag set marks nothing");
}

} // namespace

int main() {
    if (!clamping_and_dirty()) {
        return 1;
    }
    if (!hit_test_with_offset()) {
        return 2;
    }
    if (!wheel_routing()) {
        return 3;
    }
    if (!wheel_prevent_default()) {
        return 4;
    }
    if (!nested_containers()) {
        return 5;
    }
    if (!paint_offset_and_clip()) {
        return 6;
    }
    if (!damage_after_scroll()) {
        return 7;
    }
    if (!scroll_into_view_and_determinism()) {
        return 8;
    }
    if (!scrollable_flag_dirty()) {
        return 9;
    }

    std::cout << "scroll: ok\n";
    return 0;
}
