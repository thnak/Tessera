// SPDX-License-Identifier: MIT

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/paint_pass.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <stdexcept>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::RedrawPlan;
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
        std::cerr << "frame_pipeline: " << message << "\n";
    }
    return condition;
}

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

KeyframeBlock block(const char* name, const char* property, const char* from, const char* to) {
    KeyframeBlock result;
    result.name = name;
    result.steps.push_back(KeyframeStep{0.0, {Declaration{property, from, false, 0}}});
    result.steps.push_back(KeyframeStep{100.0, {Declaration{property, to, false, 0}}});
    return result;
}

KeyframeTiming timing(FillMode fill) {
    KeyframeTiming result;
    result.duration = 0.1;
    result.fill = fill;
    return result;
}

bool scissor_is(const Scissor& scissor, std::int32_t x, std::int32_t y, std::uint32_t width,
                std::uint32_t height) {
    return scissor.x == x && scissor.y == y && scissor.width == width && scissor.height == height;
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
    float radius = 0.0f;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        ops.push_back({PaintOp::Kind::Rect, rect, color, 0.0f});
    }

    void fill_rounded_rect(const Rect& rect, const Color& color, float radius) override {
        ops.push_back({PaintOp::Kind::Rounded, rect, color, radius});
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
                                 same_color(a.color, b.color) && a.radius == b.radius;
                      });
}

struct NodeSnapshot {
    NodeId id = 0;
    Rect bounds;
    Mat3 transform;
    bool visible = true;
    Dirty dirty = Dirty::Clean;
    Dirty subtree_dirty = Dirty::Clean;
    std::size_t children = 0;

    bool operator==(const NodeSnapshot& other) const {
        return id == other.id && same_rect(bounds, other.bounds) &&
               std::equal(std::begin(transform.m), std::end(transform.m),
                          std::begin(other.transform.m)) &&
               visible == other.visible && dirty == other.dirty &&
               subtree_dirty == other.subtree_dirty && children == other.children;
    }
};

void snapshot(const Node& node, std::vector<NodeSnapshot>& out) {
    out.push_back(NodeSnapshot{node.id(), node.bounds(), node.transform(), node.visible(),
                               node.dirty(), node.subtree_dirty(), node.children().size()});
    for (const auto& child : node.children()) {
        snapshot(*child, out);
    }
}

bool test_paint_frame() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    auto first_owner = std::make_unique<Node>(2);
    first_owner->set_bounds({10, 10, 20, 20});
    root.add_child(std::move(first_owner));
    auto second_owner = std::make_unique<Node>(3);
    second_owner->set_bounds({50, 50, 20, 20});
    Node& second = root.add_child(std::move(second_owner));
    clear_tree(root);

    StubStyles styles;
    styles.set(1, styled({{"background-color", "red"},
                          {"border-top-width", "2px"},
                          {"border-top-style", "solid"},
                          {"border-top-color", "black"},
                          {"border-right-width", "2px"},
                          {"border-right-style", "solid"},
                          {"border-right-color", "black"},
                          {"border-bottom-width", "2px"},
                          {"border-bottom-style", "solid"},
                          {"border-bottom-color", "black"},
                          {"border-left-width", "2px"},
                          {"border-left-style", "solid"},
                          {"border-left-color", "black"}}));
    styles.set(2, styled({{"background-color", "blue"}}));
    styles.set(3, styled({{"background-color", "green"}}));

    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{200, 200}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);

    RecordingSink sink;

    // Before any tick/finish_frame there is no frame decision, so paint_frame is a no-op.
    FramePaint paint = pipeline.paint_frame(root, styles, sink);
    if (!expect(!pipeline.frame_due(), "no frame decided yet") ||
        !expect(!paint.painted, "undecided frame skips paint") ||
        !expect(sink.ops.empty(), "undecided frame emits nothing") ||
        !expect(paint.stats.nodes == 0 && paint.stats.backgrounds == 0 && paint.stats.borders == 0,
                "skipped paint reports no stats")) {
        return false;
    }

    // An idle frame decides not to render; paint_frame still emits nothing.
    FrameTick tick = pipeline.tick(root, 0.016);
    FrameRender render = pipeline.finish_frame(root);
    paint = pipeline.paint_frame(root, styles, sink);
    if (!expect(!render.should_render, "idle frame is not due") ||
        !expect(!pipeline.frame_due(), "idle frame clears the due flag") ||
        !expect(!paint.painted && sink.ops.empty(), "idle frame paints nothing")) {
        return false;
    }

    // An animation-driven paint dirty path still decides a real frame; the paint pass then runs
    // once with the scene's box visuals in paint order.
    animations.start(AnimationInput{
        3, block("fade", "opacity", "0", "1"), timing(FillMode::None), {}, "", false});
    tick = pipeline.tick(root, 0.05);
    render = pipeline.finish_frame(root);
    if (!expect(render.should_render, "animation dirty frame is due") ||
        !expect(pipeline.frame_due(), "animation frame sets the due flag") ||
        !expect(has_flag(second.dirty(), Dirty::Paint), "animation marks the target paint dirty")) {
        return false;
    }

    std::vector<NodeSnapshot> before;
    snapshot(root, before);

    paint = pipeline.paint_frame(root, styles, sink);
    if (!expect(paint.painted, "due frame paints") ||
        !expect(paint.stats.nodes == 3, "three visible nodes") ||
        !expect(paint.stats.backgrounds == 3, "three backgrounds") ||
        !expect(paint.stats.borders == 4, "four border edges") ||
        !expect(paint.stats.hidden_subtrees == 0, "no hidden subtrees") ||
        !expect(sink.ops.size() == 7, "root background, four edges, then two children") ||
        !expect(sink.ops[0].kind == PaintOp::Kind::Rect &&
                    same_rect(sink.ops[0].rect, {0, 0, 200, 200}) &&
                    same_color(sink.ops[0].color, tessera::rendering::rhi::rgba8(255, 0, 0)),
                "root background paints first") ||
        !expect(same_rect(sink.ops[1].rect, {0, 0, 200, 2}), "top edge") ||
        !expect(same_rect(sink.ops[2].rect, {198, 2, 2, 196}), "right edge") ||
        !expect(same_rect(sink.ops[3].rect, {0, 198, 200, 2}), "bottom edge") ||
        !expect(same_rect(sink.ops[4].rect, {0, 2, 2, 196}), "left edge") ||
        !expect(same_rect(sink.ops[5].rect, {10, 10, 20, 20}) &&
                    same_color(sink.ops[5].color, tessera::rendering::rhi::rgba8(0, 0, 255)),
                "first child paints before the second") ||
        !expect(same_rect(sink.ops[6].rect, {50, 50, 20, 20}) &&
                    same_color(sink.ops[6].color, tessera::rendering::rhi::rgba8(0, 128, 0)),
                "second child paints last")) {
        return false;
    }

    std::vector<NodeSnapshot> after;
    snapshot(root, after);
    if (!expect(before == after, "paint never mutates the scene")) {
        return false;
    }

    // Painting is not a one-shot: a second call in the same frame is allowed and reproduces the
    // same commands and stats.
    const std::vector<PaintOp> first_pass = sink.ops;
    sink.ops.clear();
    paint = pipeline.paint_frame(root, styles, sink);
    if (!expect(paint.painted, "painting twice in one frame is allowed") ||
        !expect(same_ops(sink.ops, first_pass), "repeated paint is deterministic") ||
        !expect(paint.stats.nodes == 3 && paint.stats.backgrounds == 3 && paint.stats.borders == 4,
                "repeated paint stats match")) {
        return false;
    }

    // Damage-clipped paint: the due frame's plan repaints only its scissor regions.
    sink.ops.clear();
    paint = pipeline.paint_frame(root, styles, sink, render.redraw_plan);
    if (!expect(paint.painted, "partial plan paints") ||
        !expect(sink.ops.size() == 2, "partial plan clips away everything outside its region") ||
        !expect(same_rect(sink.ops[0].rect, {50, 50, 20, 20}),
                "root background is clipped to the region") ||
        !expect(same_rect(sink.ops[1].rect, {50, 50, 20, 20}),
                "the node fully inside the region paints unchanged") ||
        !expect(paint.stats.nodes == 3 && paint.stats.backgrounds == 3 && paint.stats.borders == 4,
                "stats stay the pre-clip paint pass counts")) {
        return false;
    }
    std::vector<NodeSnapshot> clipped_scene;
    snapshot(root, clipped_scene);
    if (!expect(clipped_scene == after, "clipped paint never mutates the scene")) {
        return false;
    }
    // A full-redraw plan ignores its scissor list and reproduces the full paint byte for byte.
    RedrawPlan full_plan;
    full_plan.full_redraw = true;
    full_plan.scissors = {{0, 0, 10, 10}};
    sink.ops.clear();
    paint = pipeline.paint_frame(root, styles, sink, full_plan);
    if (!expect(paint.painted && same_ops(sink.ops, first_pass),
                "a full-redraw plan equals the full paint")) {
        return false;
    }
    sink.ops.clear();
    paint = pipeline.paint_frame(root, styles, sink, RedrawPlan{});
    if (!expect(!paint.painted && sink.ops.empty() && paint.stats.nodes == 0,
                "an empty plan paints nothing") ||
        !expect(!pipeline.paint_frame(root, styles, sink, RedrawPlan{}, true).painted,
                "force cannot paint an empty plan")) {
        return false;
    }
    // DamageTracker old/new bounds: a move damages both boxes, so the clip repaints the old region
    // with the background and the new region with the node.
    animations.clear();
    clear_tree(root);
    second.set_bounds({150, 150, 20, 20});
    tick = pipeline.tick(root, 0.016);
    render = pipeline.finish_frame(root);
    sink.ops.clear();
    paint = pipeline.paint_frame(root, styles, sink, render.redraw_plan);
    if (!expect(render.should_render && !render.full_redraw &&
                    render.redraw_plan.scissors.size() == 2,
                "a move damages the previous and current bounds") ||
        !expect(paint.painted && sink.ops.size() == 3 &&
                    same_rect(sink.ops[0].rect, {50, 50, 20, 20}) &&
                    same_rect(sink.ops[1].rect, {150, 150, 20, 20}) &&
                    same_rect(sink.ops[2].rect, {150, 150, 20, 20}),
                "old region keeps the background, the new region adds the node")) {
        return false;
    }
    // The explicit force flag paints an idle frame (e.g. a caller repainting after a resize)
    // without changing the frame decision or queueing damage.
    animations.clear();
    clear_tree(root);
    tick = pipeline.tick(root, 0.016);
    render = pipeline.finish_frame(root);
    sink.ops.clear();
    if (!expect(!render.should_render, "forced paint starts from an idle frame") ||
        !expect(!pipeline.frame_due(), "idle frame due flag is clear") ||
        !expect(!pipeline.paint_frame(root, styles, sink).painted && sink.ops.empty(),
                "idle frame still paints nothing without force") ||
        !expect(pipeline.paint_frame(root, styles, sink, true).painted,
                "explicit force paints an idle frame") ||
        !expect(sink.ops.size() == 7, "forced paint still covers the full frame")) {
        return false;
    }
    RedrawPlan forced_plan;
    forced_plan.scissors = {{150, 150, 20, 20}};
    sink.ops.clear();
    paint = pipeline.paint_frame(root, styles, sink, forced_plan, true);
    if (!expect(paint.painted && sink.ops.size() == 2,
                "force also paints a partial plan on an idle frame")) {
        return false;
    }

    render = pipeline.finish_frame(root);
    if (!expect(!render.should_render && render.damage.rects == 0,
                "paint leaves damage and redraw planning unchanged")) {
        return false;
    }
    return true;
}

} // namespace

// Issue #5: a scheduled component update runs at the head of tick(), before animation, style and
// layout, so the node it dirties reaches this frame's damage; scheduling wakes the loop.
struct ScheduledNode {
    Node* node = nullptr;
    int updates = 0;
};

bool test_update_scheduler() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& child = root.add_child(std::make_unique<Node>(2));
    child.set_bounds({10, 10, 20, 20});
    clear_tree(root);

    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{200, 200}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);
    tessera::ui::UpdateScheduler scheduler;
    pipeline.set_update_scheduler(&scheduler);

    ScheduledNode scheduled{&child, 0};
    tessera::ui::UpdateTicket ticket;
    ticket.context = &scheduled;
    ticket.update = [](void* context) {
        auto* self = static_cast<ScheduledNode*>(context);
        ++self->updates;
        self->node->mark_dirty(Dirty::Paint | Dirty::Render);
    };
    scheduler.attach(ticket);

    (void)pipeline.tick(root, 0.016); // settle the first frame
    (void)pipeline.finish_frame(root);
    clear_tree(root);
    ticket.schedule();
    if (!expect(scheduled.updates == 0, "scheduling does not run the update inline") ||
        !expect(loop.should_render(), "scheduling requests a frame from the loop")) {
        return false;
    }
    (void)pipeline.tick(root, 0.016);
    const FrameRender render = pipeline.finish_frame(root);
    if (!expect(scheduled.updates == 1, "tick flushes the scheduler once") ||
        !expect(render.damage.nodes == 1 && render.should_render,
                "the scheduled update's dirty node is part of this frame's damage")) {
        return false;
    }
    (void)pipeline.tick(root, 0.016);
    pipeline.set_update_scheduler(nullptr);
    ticket.schedule();
    return expect(scheduled.updates == 1, "an uninstalled scheduler no longer flushes on tick");
}

int main() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    auto first = std::make_unique<Node>(2);
    first->set_bounds({10, 10, 20, 20});
    Node& first_ref = root.add_child(std::move(first));
    auto second = std::make_unique<Node>(3);
    second->set_bounds({50, 50, 20, 20});
    Node& second_ref = root.add_child(std::move(second));
    clear_tree(root);

    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{200, 200}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);

    animations.start(AnimationInput{
        2, block("fade", "opacity", "0", "1"), timing(FillMode::Forwards), {}, "", false});
    FrameTick tick = pipeline.tick(root, 0.05);
    FrameRender render = pipeline.finish_frame(root);
    if (!expect(tick.animation.samples.size() == 1, "opacity sample") ||
        !expect(tick.animation.keep_awake, "opacity keeps awake") ||
        !expect(!tick.needs_style && !tick.needs_layout, "opacity is paint-only") ||
        !expect(has_flag(first_ref.dirty(), Dirty::Paint), "target paint dirty") ||
        !expect(!has_flag(first_ref.dirty(), Dirty::Layout), "target layout clean") ||
        !expect(render.damage.nodes == 1 && render.damage.rects == 1, "one damaged node") ||
        !expect(render.redraw_plan.scissors.size() == 1, "one scissor") ||
        !expect(scissor_is(render.redraw_plan.scissors[0], 10, 10, 20, 20), "scissor bounds") ||
        !expect(render.should_render && !render.full_redraw, "partial render")) {
        return 1;
    }

    clear_tree(root);
    animations.clear();
    tick = pipeline.tick(root, 0.016);
    render = pipeline.finish_frame(root);
    if (!expect(tick.animation.samples.empty(), "idle has no samples") ||
        !expect(!tick.animation.keep_awake, "idle is asleep") ||
        !expect(!tick.needs_style && !tick.needs_layout, "idle needs nothing") ||
        !expect(render.damage.rects == 0 && render.damage.nodes == 0, "idle damage empty") ||
        !expect(render.redraw_plan.scissors.empty(), "idle plan empty") ||
        !expect(!render.should_render, "idle skips render")) {
        return 2;
    }

    animations.start(AnimationInput{
        2, block("grow", "width", "10px", "20px"), timing(FillMode::None), {}, "", false});
    tick = pipeline.tick(root, 0.05);
    if (!expect(tick.needs_layout, "width animation needs layout") ||
        !expect(!tick.needs_style, "width animation does not need style") ||
        !expect(has_flag(first_ref.dirty(), Dirty::Layout), "layout dirty on target")) {
        return 3;
    }
    clear_tree(root);
    animations.clear();

    animations.start(AnimationInput{
        2, block("fade", "opacity", "0", "1"), timing(FillMode::None), {}, "", false});
    animations.start(AnimationInput{
        3, block("grow", "width", "10px", "20px"), timing(FillMode::None), {}, "", false});
    tick = pipeline.tick(root, 0.05);
    if (!expect(!has_flag(first_ref.dirty(), Dirty::Layout), "paint target has no layout") ||
        !expect(has_flag(second_ref.dirty(), Dirty::Layout), "layout target keeps layout")) {
        return 4;
    }
    clear_tree(root);
    animations.clear();

    animations.start(AnimationInput{
        999, block("fade", "opacity", "0", "1"), timing(FillMode::None), {}, "", false});
    tick = pipeline.tick(root, 0.05);
    render = pipeline.finish_frame(root);
    if (!expect(tick.animation.samples.size() == 1, "unknown target still samples") ||
        !expect(render.damage.rects == 0, "unknown target damages nothing") ||
        !expect(!render.should_render, "unknown target skips render")) {
        return 5;
    }
    animations.clear();
    clear_tree(root);

    DamageAccumulator small_damage(DamageConfig{Extent2D{200, 200}, 1});
    DamageTracker small_tracker;
    DirtyPass small_dirty;
    FramePipeline small_pipeline(animations, loop, small_tracker, small_damage, small_dirty);
    first_ref.mark_dirty(Dirty::Paint);
    second_ref.mark_dirty(Dirty::Paint);
    tick = small_pipeline.tick(root, 0.016);
    render = small_pipeline.finish_frame(root);
    if (!expect(!tick.needs_style && !tick.needs_layout, "manual paint dirties only damage") ||
        !expect(render.full_redraw, "damage overflow forces full redraw") ||
        !expect(render.redraw_plan.scissors.size() == 1, "full redraw has one scissor") ||
        !expect(scissor_is(render.redraw_plan.scissors[0], 0, 0, 200, 200),
                "full redraw covers target") ||
        !expect(render.should_render, "full redraw renders")) {
        return 6;
    }
    clear_tree(root);

    const auto rejects = [&](FramePipelineConfig config) {
        try {
            FramePipeline invalid(animations, loop, tracker, damage, dirty, config);
            (void)invalid;
        } catch (const std::invalid_argument&) {
            return true;
        }
        return false;
    };
    if (!expect(rejects(FramePipelineConfig{0.0f, 0.0f, {}}), "zero dpi rejected")) {
        return 7;
    }
    if (!expect(rejects(FramePipelineConfig{1.0f, -1.0f, {}}), "negative margin rejected")) {
        return 8;
    }

    if (!test_paint_frame()) {
        return 9;
    }
    if (!test_update_scheduler()) {
        return 10;
    }

    return 0;
}
