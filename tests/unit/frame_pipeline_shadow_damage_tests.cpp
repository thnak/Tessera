// SPDX-License-Identifier: MIT
//
// A `box-shadow` transition (issue #462) changes the shadow's reach every frame. The damage of a
// frame has to cover the reach the shadow had on the previous frame as well as the new one, or a
// shrinking blur leaves stale shadow pixels behind.

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"

#include <iostream>
#include <map>
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
using tessera::ui::NodeId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FrameTick;
using tessera::ui::layout::DamageTracker;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "frame_pipeline_shadow_damage: " << message << "\n";
    }
    return condition;
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

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

} // namespace

int main() {
    Node root(1);
    root.set_bounds({0, 0, 400, 400});
    Node& box = root.add_child(std::make_unique<Node>(2));
    box.set_bounds({150, 150, 50, 50});
    clear_tree(root);

    StubStyles styles;
    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{400, 400}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);

    auto step = [&](const char* shadow) {
        ComputedStyle style;
        style.set("box-shadow", shadow);
        styles.set(2, style);
        box.mark_dirty(Dirty::Style);
        FrameTick tick = pipeline.tick(root, 0.016);
        pipeline.sync_style(root, styles, tick);
        const auto render = pipeline.finish_frame(root);
        clear_tree(root);
        return render;
    };

    // Frame 1: a wide shadow (reach: blur 40 px, so the box grows by about 40 on every side).
    step("0 0 40px 0 #000");
    // Frame 2: the blur shrinks the way a transition's later ticks do.
    const auto shrunk = step("0 0 8px 0 #000");

    // The old reach (150 - 40 = 110 on the left, 200 + 40 = 240 on the right) must be repainted.
    bool covers_old_reach = false;
    for (const auto& scissor : shrunk.redraw_plan.scissors) {
        covers_old_reach = covers_old_reach ||
                           (scissor.x <= 112 && scissor.x + static_cast<int>(scissor.width) >= 238);
    }
    if (!expect(shrunk.should_render, "a shrinking shadow makes a frame due") ||
        !expect(covers_old_reach, "damage covers the previous, wider shadow reach")) {
        return 1;
    }
    return 0;
}
