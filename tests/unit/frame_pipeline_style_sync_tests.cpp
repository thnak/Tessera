// SPDX-License-Identifier: MIT
//
// FramePipeline::sync_style: runs only when the tick asked for style/layout work, applies
// transform / z-index / box-shadow reach before damage is derived, and makes a style-only change
// (a new transform or shadow) produce a due frame whose damage covers the shadow.

#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"

#include <cmath>
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
        std::cerr << "frame_pipeline_style_sync: " << message << "\n";
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

bool sync_runs_only_on_style_or_layout_work_and_reaches_damage() {
    Node root(1);
    root.set_bounds({0, 0, 400, 400});
    Node& box = root.add_child(std::make_unique<Node>(2));
    box.set_bounds({100, 100, 50, 50});
    clear_tree(root);

    StubStyles styles;
    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{400, 400}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);

    // Prime the damage tracker so the next frame reports only what changes.
    box.mark_dirty(Dirty::Paint | Dirty::Render);
    pipeline.tick(root, 0.016);
    pipeline.finish_frame(root);
    clear_tree(root);

    ComputedStyle shadowed;
    shadowed.set("box-shadow", "0 0 10px #000");
    shadowed.set("z-index", "4");
    shadowed.set("transform", "translate(20px, 0)");
    styles.set(2, shadowed);

    // A quiet tick (no style/layout work) skips the sync entirely.
    FrameTick quiet = pipeline.tick(root, 0.016);
    const auto skipped = pipeline.sync_style(root, styles, quiet);
    bool ok = expect(skipped.nodes == 0 && box.z_index() == 0, "no style work, no sync");
    pipeline.finish_frame(root);
    clear_tree(root);

    // A style-dirty tick runs it, and the visual changes reach damage.
    box.mark_dirty(Dirty::Style);
    FrameTick tick = pipeline.tick(root, 0.016);
    ok = expect(tick.needs_style, "style work reported") && ok;
    const auto synced = pipeline.sync_style(root, styles, tick);
    ok = expect(synced.changed_z_index == 1 && synced.changed_transform == 1 &&
                    synced.changed_outsets == 1,
                "z-index, transform and shadow reach were applied") &&
         ok;
    const auto render = pipeline.finish_frame(root);
    ok = expect(render.should_render, "a style-only transform/shadow change makes a frame due") &&
         ok;
    // Old box (100..150, no shadow yet) and the moved box (120..170) plus its shadow reach (10):
    // damage must span x = 100 on the left to x = 180 on the right.
    bool reaches = false;
    for (const auto& scissor : render.redraw_plan.scissors) {
        reaches =
            reaches || (scissor.x <= 100 && scissor.x + static_cast<int>(scissor.width) >= 180);
    }
    ok = expect(reaches && !render.full_redraw,
                "partial damage covers the shadow and the moved box") &&
         ok;
    return ok;
}

bool forced_sync_runs_on_a_quiet_tick() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    StubStyles styles;
    ComputedStyle style;
    style.set("z-index", "2");
    styles.set(1, style);
    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{100, 100}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline(animations, loop, tracker, damage, dirty);
    clear_tree(root);
    FrameTick quiet = pipeline.tick(root, 0.016);
    pipeline.sync_style(root, styles, quiet, true);
    return expect(root.z_index() == 2, "force overrides the quiet-tick skip");
}

} // namespace

int main() {
    bool ok = true;
    ok = sync_runs_only_on_style_or_layout_work_and_reaches_damage() && ok;
    ok = forced_sync_runs_on_a_quiet_tick() && ok;
    if (!ok) {
        std::cerr << "frame_pipeline_style_sync: FAILED\n";
        return 1;
    }
    std::cout << "frame_pipeline_style_sync: ok\n";
    return 0;
}
