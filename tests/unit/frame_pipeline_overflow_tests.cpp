// SPDX-License-Identifier: MIT
//
// Dirty-pass overflow through the frame pipeline (task-65): a frame with more dirty nodes than the
// DirtyPass cap must be a full redraw that paints exactly like a full redraw, the next under-cap
// frame must return to partial damage, and settling must still be an idle frame.
//
// Plus the frame-level half of paint-only interpolation (task-91): a render between two ticks
// paints the blended transform while the scene graph, damage and hit testing keep the tick
// geometry, the interpolation never asks for layout work, changing alpha alone never makes a frame
// due, and the same scripted two-tick scene paints the same commands every run.

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_interpolation.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Mat3;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::Scissor;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::frame::FramePaint;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FramePipelineConfig;
using tessera::ui::frame::FrameRender;
using tessera::ui::frame::FrameTick;
using tessera::ui::layout::DamageTracker;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

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

// --- Paint-only interpolation through the frame pipeline -----------------------------------------

KeyframeBlock slide_block() {
    KeyframeBlock block;
    block.name = "slide";
    // A transform list is not numerically interpolated by the keyframe sampler (only numbers and
    // colors are), so the sampled value steps at the halfway point: tick 1 carries translateX(0),
    // tick 2 translateX(40). The paint pass is what makes the motion smooth between them.
    block.steps.push_back(
        KeyframeStep{0.0, {Declaration{"transform", "translateX(0px)", false, 0}}});
    block.steps.push_back(
        KeyframeStep{100.0, {Declaration{"transform", "translateX(40px)", false, 0}}});
    return block;
}

KeyframeTiming slide_timing() {
    KeyframeTiming timing;
    timing.duration = 0.2;
    return timing;
}

// One scripted two-tick scene, painted at `alpha`. The scene's own transform is set to the second
// tick's value before that tick (what a caller syncing style does), so the authoritative tick
// geometry and the interpolated paint position are different by construction.
struct InterpScript {
    std::vector<PaintOp> ops;
    Rect child;
    bool painted = false;
    bool rendered = false;
    bool needs_layout = false;
    Dirty child_dirty = Dirty::Clean;
    bool hit_current_tick = false;
    bool hit_interpolated = false;
    bool scene_unmoved = false;
    bool plan_covers_paint = false;
};

InterpScript run_two_tick_script(double alpha, bool interpolation = true) {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& box = root.add_child(std::make_unique<Node>(2));
    box.set_bounds({10, 10, 20, 20});
    clear_tree(root);

    StubStyles styles;
    styles.set(1, styled({{"background-color", "red"}}));
    styles.set(2, styled({{"background-color", "blue"}}));

    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{200, 200}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty,
                           FramePipelineConfig{1.0f, 0.0f, {}, interpolation});

    animations.start(AnimationInput{2, slide_block(), slide_timing(), {}, "", false});
    pipeline.tick(root, 0.05, alpha); // samples translateX(0)
    pipeline.finish_frame(root);

    const Mat3 authoritative = Mat3::translation(40.0f, 0.0f);
    box.set_transform(authoritative);
    clear_tree(root);

    InterpScript result;
    const FrameTick tick = pipeline.tick(root, 0.1, alpha); // samples translateX(40)
    const FrameRender render = pipeline.finish_frame(root);
    result.rendered = render.should_render;
    result.needs_layout = tick.needs_layout;
    result.child_dirty = box.dirty();

    // A force paint skips the damage scissors: this test asks where the box is painted, not what is
    // damaged.
    RecordingSink sink;
    const FramePaint paint = pipeline.paint_frame(root, styles, sink, true);
    result.painted = paint.painted;
    result.ops = sink.ops;
    if (!result.ops.empty()) {
        result.child = result.ops.back().rect;
    }

    // The redraw plan must contain the interpolated paint box in one region: the tick damage alone
    // covers only the two tick positions, not the space between them.
    if (result.child.width > 0.0f) {
        for (const Scissor& region : render.redraw_plan.scissors) {
            const float right = static_cast<float>(region.x) + static_cast<float>(region.width);
            const float bottom = static_cast<float>(region.y) + static_cast<float>(region.height);
            if (static_cast<float>(region.x) <= result.child.x &&
                static_cast<float>(region.y) <= result.child.y && right >= result.child.right() &&
                bottom >= result.child.bottom()) {
                result.plan_covers_paint = true;
                break;
            }
        }
    }

    // Hit testing runs after painting and only sees the tick geometry: the box's authoritative
    // transform is the 40px offset, so it is hit there and not at the interpolated position.
    result.hit_current_tick = root.hit_test_deep(60.0f, 15.0f) == &box;
    result.hit_interpolated = root.hit_test_deep(35.0f, 15.0f) == &box;
    result.scene_unmoved = same_rect(box.bounds(), Rect{10, 10, 20, 20}) &&
                           std::equal(std::begin(authoritative.m), std::end(authoritative.m),
                                      std::begin(box.transform().m));
    return result;
}

bool paint_interpolates_between_ticks() {
    const InterpScript start = run_two_tick_script(0.0);
    const InterpScript middle = run_two_tick_script(0.5);
    const InterpScript end = run_two_tick_script(1.0);
    bool ok = expect(start.painted && middle.painted && end.painted, "every alpha paints");
    ok = expect(same_rect(start.child, Rect{10, 10, 20, 20}),
                "alpha 0 paints the previous tick sample") &&
         ok;
    ok = expect(same_rect(middle.child, Rect{30, 10, 20, 20}),
                "alpha 0.5 paints halfway between the ticks") &&
         ok;
    ok = expect(same_rect(end.child, Rect{50, 10, 20, 20}),
                "alpha 1 paints the current tick sample") &&
         ok;
    // Partial damage must cover the interpolated box even though it lies between the two tick
    // positions, otherwise a damage-clipped repaint would leave stale pixels.
    ok = expect(middle.rendered && middle.plan_covers_paint,
                "the redraw plan covers the interpolated paint box") &&
         ok;
    // The root's own geometry is untouched by the child's override.
    ok = expect(middle.ops.size() == 2 && same_rect(middle.ops.front().rect, Rect{0, 0, 200, 200}),
                "only the interpolated subtree moves") &&
         ok;
    // A transform sample is paint/render work: layout is never requested for it.
    ok = expect(!middle.needs_layout && !has_flag(middle.child_dirty, Dirty::Layout) &&
                    has_flag(middle.child_dirty, Dirty::Paint) &&
                    has_flag(middle.child_dirty, Dirty::Render),
                "interpolation dirties paint/render, never layout") &&
         ok;
    // Interpolation is opt-out: with it disabled the frame paints the tick's own transform.
    const InterpScript disabled = run_two_tick_script(0.5, false);
    ok = expect(same_rect(disabled.child, Rect{50, 10, 20, 20}),
                "interpolation disabled paints the authoritative transform") &&
         ok;
    return ok;
}

bool interpolation_keeps_tick_geometry() {
    const InterpScript middle = run_two_tick_script(0.5);
    bool ok = expect(middle.scene_unmoved, "painting an interpolated frame never moves the scene");
    ok = expect(middle.hit_current_tick, "hit testing uses the current tick geometry") && ok;
    ok = expect(!middle.hit_interpolated, "hit testing ignores the interpolated paint position") &&
         ok;
    return ok;
}

bool interpolation_never_wakes_a_settled_scene() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& box = root.add_child(std::make_unique<Node>(2));
    box.set_bounds({10, 10, 20, 20});
    clear_tree(root);

    StubStyles styles;
    styles.set(2, styled({{"background-color", "blue"}}));

    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{200, 200}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);

    animations.start(AnimationInput{2, slide_block(), slide_timing(), {}, "", false});
    pipeline.tick(root, 0.05, 0.0);
    pipeline.finish_frame(root);
    animations.clear();
    clear_tree(root);

    // Changing alpha alone is not a wake source: interpolation applies to frames that damage (or a
    // render request) already made due. A settled scene therefore stays at zero presents.
    const double alphas[] = {0.0, 0.25, 0.75, 1.0};
    for (const double alpha : alphas) {
        const FrameTick tick = pipeline.tick(root, 0.016, alpha);
        const FrameRender render = pipeline.finish_frame(root);
        RecordingSink sink;
        if (!expect(!tick.needs_layout && !tick.needs_style, "a settled tick needs no work") ||
            !expect(!render.should_render && render.redraw_plan.scissors.empty(),
                    "alpha alone never makes a frame due") ||
            !expect(!pipeline.paint_frame(root, styles, sink).painted && sink.ops.empty(),
                    "a settled scene paints nothing at any alpha")) {
            return false;
        }
    }
    return true;
}

bool interpolation_is_deterministic() {
    const InterpScript first = run_two_tick_script(0.5);
    const InterpScript second = run_two_tick_script(0.5);
    return expect(first.ops.size() == second.ops.size() && same_ops(first.ops, second.ops) &&
                      same_rect(first.child, second.child),
                  "the same two-tick script paints identical commands");
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

    if (!paint_interpolates_between_ticks()) {
        return 6;
    }
    if (!interpolation_keeps_tick_geometry()) {
        return 7;
    }
    if (!interpolation_never_wakes_a_settled_scene()) {
        return 8;
    }
    if (!interpolation_is_deterministic()) {
        return 9;
    }

    std::cout << "frame_pipeline_overflow: ok\n";
    return 0;
}
