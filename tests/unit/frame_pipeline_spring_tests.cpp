// SPDX-License-Identifier: MIT

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Extent2D;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::SpringAnimationInput;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FrameTick;
using tessera::ui::layout::DamageTracker;

constexpr double kDt = 1.0 / 60.0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "frame_pipeline_spring: " << message << "\n";
    }
    return condition;
}

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

SpringAnimationInput opacity(std::uint64_t target, float from, float to) {
    SpringAnimationInput input;
    input.target = target;
    input.property = "opacity";
    input.from = from;
    input.to = to;
    return input;
}

struct Fixture {
    Fixture()
        : root(1),
          loop(clock),
          damage(DamageConfig{Extent2D{200, 200}, 8}),
          pipeline(animations, loop, tracker, damage, dirty) {
        root.set_bounds({0, 0, 200, 200});
        root.add_child(std::make_unique<Node>(2)).set_bounds({10, 10, 20, 20});
        clear_tree(root);
    }

    // Ticks until the spring set empties; returns the tick count, or -1 when it never settles.
    int run_until_settled(int limit = 600) {
        for (int step = 0; step < limit; ++step) {
            (void)pipeline.tick(root, kDt);
            (void)pipeline.finish_frame(root);
            clear_tree(root);
            if (pipeline.springs().empty()) {
                return step + 1;
            }
        }
        return -1;
    }

    Node root;
    ManualClock clock;
    RuntimeLoop loop;
    AnimationRuntime animations;
    DamageAccumulator damage;
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline;
};

bool test_advances_and_settles() {
    Fixture fx;
    (void)fx.pipeline.springs().start(opacity(2, 0.0f, 1.0f));
    const FrameTick tick = fx.pipeline.tick(fx.root, kDt);
    if (!expect(tick.animation.samples.size() == 1, "spring sample merged into the tick") ||
        !expect(tick.animation.keep_awake, "spring keeps the tick awake") ||
        !expect(tick.animation.dirty != Dirty::Clean, "spring sample reports dirty") ||
        !expect(!tick.needs_style && !tick.needs_layout, "opacity spring is paint-only") ||
        !expect(fx.root.children().front()->dirty() != Dirty::Clean,
                "spring marked its node dirty")) {
        return false;
    }
    (void)fx.pipeline.finish_frame(fx.root);
    clear_tree(fx.root);
    return expect(fx.run_until_settled() > 0, "spring settles");
}

bool test_wake_only_while_active() {
    Fixture fx;
    (void)fx.pipeline.springs().start(opacity(2, 0.0f, 1.0f));
    (void)fx.pipeline.tick(fx.root, kDt);
    if (!expect(fx.loop.should_render(), "active spring requests a frame") ||
        !expect(fx.run_until_settled() > 0, "spring settles")) {
        return false;
    }
    // The settle tick requests one last frame (to present the final value); a later static tick
    // must not request another.
    fx.loop.reset();
    const FrameTick idle = fx.pipeline.tick(fx.root, kDt);
    return expect(idle.animation.samples.empty() && !idle.animation.keep_awake,
                  "no sample after settle") &&
           expect(!fx.loop.should_render(), "no wake after settle");
}

bool test_reduced_motion_snaps() {
    Fixture fx;
    fx.animations.set_reduced_motion(true);
    (void)fx.pipeline.springs().start(opacity(2, 0.0f, 1.0f));
    const FrameTick tick = fx.pipeline.tick(fx.root, kDt);
    if (!expect(tick.animation.samples.size() == 1 &&
                    tick.animation.samples[0].declarations[0].value == "1",
                "reduced motion snaps to the target on the first tick") ||
        !expect(fx.pipeline.springs().empty(), "snapped spring is finished")) {
        return false;
    }

    Fixture live;
    (void)live.pipeline.springs().start(opacity(2, 0.0f, 1.0f));
    (void)live.pipeline.tick(live.root, kDt);
    live.animations.set_reduced_motion(true);
    const FrameTick snapped = live.pipeline.tick(live.root, kDt);
    return expect(snapped.animation.samples.size() == 1 &&
                      snapped.animation.samples[0].declarations[0].value == "1",
                  "reduced motion turned on mid-flight snaps running springs") &&
           expect(live.pipeline.springs().empty(), "mid-flight snap finishes the spring");
}

bool test_removed_node_cleans_up() {
    Fixture fx;
    (void)fx.pipeline.springs().start(opacity(2, 0.0f, 1.0f));
    (void)fx.pipeline.springs().start(opacity(1, 0.0f, 1.0f));
    (void)fx.pipeline.tick(fx.root, kDt);
    (void)fx.root.remove_child(*fx.root.children().front());
    const FrameTick tick = fx.pipeline.tick(fx.root, kDt);
    return expect(fx.pipeline.springs().active() == 1, "spring on the removed node is dropped") &&
           expect(tick.animation.samples.size() == 1 && tick.animation.samples[0].target == 1,
                  "only the surviving node is sampled");
}

} // namespace

int main() {
    bool ok = true;
    ok = test_advances_and_settles() && ok;
    ok = test_wake_only_while_active() && ok;
    ok = test_reduced_motion_snaps() && ok;
    ok = test_removed_node_cleans_up() && ok;
    return ok ? 0 : 1;
}
