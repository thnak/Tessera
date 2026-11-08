// SPDX-License-Identifier: MIT

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Scissor;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::FillMode;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FramePipelineConfig;
using tessera::ui::frame::FrameRender;
using tessera::ui::frame::FrameTick;
using tessera::ui::layout::DamageTracker;
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

} // namespace

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

    bool rejected = false;
    try {
        FramePipeline invalid(animations, loop, tracker, damage, dirty,
                              FramePipelineConfig{0.0f, 0.0f, {}});
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!expect(rejected, "zero dpi rejected")) {
        return 7;
    }

    rejected = false;
    try {
        FramePipeline invalid(animations, loop, tracker, damage, dirty,
                              FramePipelineConfig{1.0f, -1.0f, {}});
        (void)invalid;
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    if (!expect(rejected, "negative margin rejected")) {
        return 8;
    }

    return 0;
}
