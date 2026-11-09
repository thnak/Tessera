// SPDX-License-Identifier: MIT
//
// Dirty-pass overflow through the frame pipeline (task-65): a frame with more dirty nodes than the
// DirtyPass cap must be a full redraw that paints exactly like a full redraw, the next under-cap
// frame must return to partial damage, and settling must still be an idle frame.

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

#include <algorithm>
#include <cstdint>
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
using tessera::rendering::rhi::Scissor;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::frame::FramePaint;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FrameRender;
using tessera::ui::frame::FrameTick;
using tessera::ui::layout::DamageTracker;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "frame_pipeline_overflow: " << message << "\n";
    }
    return condition;
}

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
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

struct PaintOp {
    enum class Kind { Rect, Rounded };

    Kind kind = Kind::Rect;
    Rect rect;
    Color color;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        ops.push_back({PaintOp::Kind::Rect, rect, color});
    }

    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        ops.push_back({PaintOp::Kind::Rounded, rect, color});
    }

    std::vector<PaintOp> ops;
};

bool same_rect(const Rect& actual, const Rect& expected) {
    return actual.x == expected.x && actual.y == expected.y && actual.width == expected.width &&
           actual.height == expected.height;
}

bool same_color(const Color& actual, const Color& expected) {
    return actual.r == expected.r && actual.g == expected.g && actual.b == expected.b &&
           actual.a == expected.a;
}

bool same_ops(const std::vector<PaintOp>& actual, const std::vector<PaintOp>& expected) {
    return std::equal(actual.begin(), actual.end(), expected.begin(), expected.end(),
                      [](const PaintOp& a, const PaintOp& b) {
                          return a.kind == b.kind && same_rect(a.rect, b.rect) &&
                                 same_color(a.color, b.color);
                      });
}

bool scissor_is(const Scissor& scissor, std::int32_t x, std::int32_t y, std::uint32_t width,
                std::uint32_t height) {
    return scissor.x == x && scissor.y == y && scissor.width == width && scissor.height == height;
}

} // namespace

int main() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    auto first_owner = std::make_unique<Node>(2);
    first_owner->set_bounds({10, 10, 20, 20});
    Node& first = root.add_child(std::move(first_owner));
    auto second_owner = std::make_unique<Node>(3);
    second_owner->set_bounds({80, 10, 20, 20});
    Node& second = root.add_child(std::move(second_owner));
    auto third_owner = std::make_unique<Node>(4);
    third_owner->set_bounds({10, 80, 20, 20});
    Node& third = root.add_child(std::move(third_owner));
    clear_tree(root);

    StubStyles styles;
    styles.set(1, styled({{"background-color", "red"}}));
    styles.set(2, styled({{"background-color", "blue"}}));
    styles.set(3, styled({{"background-color", "green"}}));
    styles.set(4, styled({{"background-color", "yellow"}}));

    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{200, 200}, 8});
    DamageTracker tracker;
    // Three dirty nodes with a two-node dirty-pass cap: the collected list is truncated.
    DirtyPass dirty(2);
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);

    first.mark_dirty(Dirty::Paint);
    second.mark_dirty(Dirty::Paint);
    third.mark_dirty(Dirty::Paint);
    const FrameTick tick = pipeline.tick(root, 0.016);
    FrameRender render = pipeline.finish_frame(root);
    if (!expect(tick.needs_style && tick.needs_layout,
                "a dirty overflow requests the conservative full layout pass") ||
        !expect(render.damage.overflowed, "the overflow is surfaced in the frame result") ||
        !expect(render.damage.full_redraw && render.redraw_plan.full_redraw,
                "an overflow frame is a full redraw") ||
        !expect(render.redraw_plan.scissors.size() == 1 &&
                    scissor_is(render.redraw_plan.scissors[0], 0, 0, 200, 200),
                "the overflow frame covers the whole target") ||
        !expect(render.should_render, "the overflow frame renders")) {
        return 1;
    }

    RecordingSink damaged;
    const FramePaint painted = pipeline.paint_frame(root, styles, damaged, render.redraw_plan);
    RecordingSink full;
    tessera::ui::paint::paint_scene(root, styles, full);
    if (!expect(painted.painted, "the overflow frame paints") ||
        !expect(same_ops(damaged.ops, full.ops),
                "an overflow frame paints exactly like a full redraw")) {
        return 2;
    }

    // Once the dirty set fits under the cap, the next frame is partial again (no latch).
    clear_tree(root);
    second.mark_dirty(Dirty::Paint);
    pipeline.tick(root, 0.016);
    render = pipeline.finish_frame(root);
    if (!expect(!render.damage.overflowed && !render.full_redraw,
                "an under-cap frame returns to partial damage") ||
        !expect(render.redraw_plan.scissors.size() == 1 &&
                    scissor_is(render.redraw_plan.scissors[0], 80, 10, 20, 20),
                "the partial region is the dirty node")) {
        return 3;
    }

    // Settling again is a true idle frame: no plan, no render (zero presents after settling).
    clear_tree(root);
    pipeline.tick(root, 0.016);
    render = pipeline.finish_frame(root);
    if (!expect(!render.should_render, "a settled frame is idle") ||
        !expect(render.redraw_plan.scissors.empty(), "an idle frame has an empty plan") ||
        !expect(!pipeline.frame_due(), "an idle frame clears the due flag")) {
        return 4;
    }

    // Determinism: the same overflow script reproduces the plan and the draw records.
    clear_tree(root);
    first.mark_dirty(Dirty::Paint);
    second.mark_dirty(Dirty::Paint);
    third.mark_dirty(Dirty::Paint);
    pipeline.tick(root, 0.016);
    const FrameRender repeated = pipeline.finish_frame(root);
    RecordingSink repeated_sink;
    pipeline.paint_frame(root, styles, repeated_sink, repeated.redraw_plan);
    if (!expect(repeated.damage.full_redraw && repeated.redraw_plan.full_redraw,
                "repeated overflow is full again") ||
        !expect(repeated.redraw_plan.scissors.size() == 1 &&
                    scissor_is(repeated.redraw_plan.scissors[0], 0, 0, 200, 200),
                "repeated overflow plan is deterministic") ||
        !expect(same_ops(repeated_sink.ops, full.ops),
                "repeated overflow paint records are deterministic")) {
        return 5;
    }

    std::cout << "frame_pipeline_overflow: ok\n";
    return 0;
}
