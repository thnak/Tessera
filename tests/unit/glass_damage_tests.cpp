// SPDX-License-Identifier: MIT
//
// Glass damage and idle rules (issue #244, docs/specs/glass-v1.md sections 7 and 9): the style sync
// resolves the sigma onto the scene, a glass node's dependency region is its box grown by 3 sigma,
// damage intersecting that region repaints the glass box while damage outside it does not, a static
// glass screen settles to zero frames, a tree without a glass node keeps its old damage, and the
// glass diagnostics counters are recorded.

#include "tessera/runtime/diagnostics.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/layout/damage_pass.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/paint/scene_style_sync.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::gpu::GlassLimits;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::rendering::rhi::Scissor;
using tessera::runtime::FrameDiagnostics;
using tessera::runtime::FrameDiagnosticsSample;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::frame::FramePaint;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FramePipelineConfig;
using tessera::ui::frame::FrameRender;
using tessera::ui::frame::FrameTick;
using tessera::ui::layout::DamageTracker;
using tessera::ui::layout::node_glass_dependency_rect;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::SceneStyleSyncResult;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::sync_scene_style;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "glass_damage: " << message << "\n";
    }
    return condition;
}

bool same_rect(const Rect& lhs, const Rect& rhs) {
    return lhs.x == rhs.x && lhs.y == rhs.y && lhs.width == rhs.width && lhs.height == rhs.height;
}

bool near(float value, float expected) {
    return value == expected;
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

ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

void clear_tree(Node& node) {
    node.clear_dirty(Dirty::All);
    for (const auto& child : node.children()) {
        clear_tree(*child);
    }
}

// root(1) with a glass child(2) and a far sibling(3) that is outside the dependency region.
struct Scene {
    Node root{1};
    StubStyles styles;
    GlassLimits limits;

    explicit Scene(bool glass) {
        root.set_bounds({0.0f, 0.0f, 200.0f, 200.0f});
        auto card = std::make_unique<Node>(2);
        card->set_bounds({50.0f, 50.0f, 40.0f, 40.0f});
        root.add_child(std::move(card));
        auto far = std::make_unique<Node>(3);
        far->set_bounds({10.0f, 10.0f, 5.0f, 5.0f});
        root.add_child(std::move(far));

        styles.set(1, styled({{"background-color", "#101010"}}));
        styles.set(2, styled({{"background-color", "rgba(255, 255, 255, 0.15)"},
                              {"backdrop-filter", glass ? "blur(4px)" : "none"}}));
        styles.set(3, styled({{"background-color", "#ff0000"}}));
        clear_tree(root);
    }
};

bool style_sync() {
    Scene scene(true);
    SceneStyleSyncResult result = sync_scene_style(scene.root, scene.styles, scene.limits);
    bool ok = expect(result.glass_nodes == 1, "one glass node is discovered");
    ok = expect(result.changed_glass == 1, "the first sync reports the sigma change") && ok;
    const Node& card = *scene.root.children().front();
    ok = expect(near(card.glass_blur_sigma(), 4.0f), "the blur sigma lands on the node") && ok;
    ok = expect(scene.root.glass_subtree() && card.glass_subtree(),
                "the subtree flag reaches the root") &&
         ok;

    // A steady scene does not re-report the same value.
    const SceneStyleSyncResult steady = sync_scene_style(scene.root, scene.styles, scene.limits);
    ok = expect(steady.changed_glass == 0, "a steady sync reports no glass change") && ok;

    // The configured clamp bounds the stored sigma, which is what damage and paint use.
    scene.styles.set(2, styled({{"backdrop-filter", "blur(200px)"}}));
    scene.limits.max_blur_radius = 20.0f;
    sync_scene_style(scene.root, scene.styles, scene.limits);
    ok = expect(near(card.glass_blur_sigma(), 20.0f), "sync clamps the sigma to the config") && ok;

    // A tree without glass reports no glass anywhere.
    Scene plain(false);
    const SceneStyleSyncResult none = sync_scene_style(plain.root, plain.styles, plain.limits);
    ok = expect(none.glass_nodes == 0 && !plain.root.glass_subtree(),
                "a non-glass tree has no glass subtree") &&
         ok;
    return ok;
}

bool dependency_region() {
    Scene scene(true);
    sync_scene_style(scene.root, scene.styles, scene.limits);
    const Node& card = *scene.root.children().front();
    const Rect region = node_glass_dependency_rect(card, 1.0f);
    // Card box (50,50,40,40) grown by 3 * sigma 4 = 12 on every side.
    return expect(same_rect(region, Rect{38.0f, 38.0f, 64.0f, 64.0f}),
                  "the dependency region is the box grown by 3 sigma");
}

bool tracker_adds_glass_boxes() {
    Scene scene(true);
    sync_scene_style(scene.root, scene.styles, scene.limits);
    DamageAccumulator damage(DamageConfig{Extent2D{200, 200}, 16});
    DamageTracker tracker;

    // Damage far outside the dependency region does not dirty the glass node.
    damage.add({10.0f, 10.0f, 5.0f, 5.0f});
    bool ok = expect(tracker.add_glass_dependencies(scene.root, damage, 1.0f, 0.0f) == 0,
                     "damage outside the 3 sigma region leaves glass alone");

    // Damage inside the region but outside the glass box still repaints the glass box.
    damage.add({36.0f, 60.0f, 4.0f, 4.0f});
    ok = expect(tracker.add_glass_dependencies(scene.root, damage, 1.0f, 0.0f) == 1,
                "damage in the dependency region repaints the glass node") &&
         ok;
    bool box_present = false;
    for (const Rect& rect : damage.rects()) {
        box_present = box_present || same_rect(rect, Rect{50.0f, 50.0f, 40.0f, 40.0f});
    }
    ok = expect(box_present, "the glass node's full box is added to damage") && ok;

    // An empty damage set costs nothing.
    DamageAccumulator empty(DamageConfig{Extent2D{200, 200}, 16});
    ok = expect(tracker.add_glass_dependencies(scene.root, empty, 1.0f, 0.0f) == 0,
                "no damage means no glass dependency") &&
         ok;
    return ok;
}

struct Pipeline {
    ManualClock clock;
    RuntimeLoop loop{clock};
    AnimationRuntime animations;
    DamageAccumulator damage{DamageConfig{Extent2D{200, 200}, 16}};
    DamageTracker tracker;
    DirtyPass dirty;
    FramePipeline pipeline{animations, loop, tracker, damage, dirty, FramePipelineConfig{}};
};

bool find_glass_scissor(const FrameRender& render) {
    for (const Scissor& scissor : render.redraw_plan.scissors) {
        const bool covers = scissor.x <= 50 && scissor.y <= 50 &&
                            scissor.x + static_cast<std::int32_t>(scissor.width) >= 90 &&
                            scissor.y + static_cast<std::int32_t>(scissor.height) >= 90;
        if (covers) {
            return true;
        }
    }
    return false;
}

bool frame_pipeline_idle_and_damage() {
    Scene scene(true);
    Pipeline state;
    state.pipeline.sync_style(scene.root, scene.styles, state.pipeline.tick(scene.root, 1.0 / 60.0),
                              true);
    // The first frame paints the whole scene.
    FrameRender first = state.pipeline.finish_frame(scene.root);
    bool ok = expect(first.should_render, "the first frame renders");
    ok = expect(first.glass_dependencies == 1, "the first glass frame accounts a dependency") && ok;

    // Settle: a static glass screen presents zero frames (glass-v1 section 9 / performance.md 9).
    clear_tree(scene.root);
    state.pipeline.tick(scene.root, 1.0 / 60.0);
    const FrameRender idle = state.pipeline.finish_frame(scene.root);
    ok = expect(!idle.should_render && idle.damage.rects == 0,
                "a static glass screen presents zero frames") &&
         ok;
    ok = expect(idle.glass_dependencies == 0, "idle frames add no glass dependency") && ok;

    // A change under the glass node repaints the glass box.
    scene.root.mark_dirty(Dirty::Paint | Dirty::Render);
    state.pipeline.tick(scene.root, 1.0 / 60.0);
    const FrameRender changed = state.pipeline.finish_frame(scene.root);
    ok = expect(changed.should_render, "a change under glass repaints") && ok;
    ok = expect(changed.glass_dependencies == 1, "the changed glass node is repainted") && ok;
    ok = expect(find_glass_scissor(changed), "damage covers the glass box") && ok;

    // A change outside the dependency region does not dirty the glass node.
    clear_tree(scene.root);
    state.pipeline.tick(scene.root, 1.0 / 60.0);
    (void)state.pipeline.finish_frame(scene.root);
    scene.root.children().back()->mark_dirty(Dirty::Paint | Dirty::Render);
    state.pipeline.tick(scene.root, 1.0 / 60.0);
    const FrameRender outside = state.pipeline.finish_frame(scene.root);
    ok = expect(outside.should_render, "the far change repaints itself") && ok;
    ok = expect(outside.glass_dependencies == 0, "damage outside the region leaves glass alone") &&
         ok;
    ok = expect(outside.damage.rects == 1, "only the changed node is damaged") && ok;
    return ok;
}

bool non_glass_damage_is_unchanged() {
    Scene scene(false);
    Pipeline state;
    state.pipeline.sync_style(scene.root, scene.styles, state.pipeline.tick(scene.root, 1.0 / 60.0),
                              true);
    (void)state.pipeline.finish_frame(scene.root);
    clear_tree(scene.root);
    state.pipeline.tick(scene.root, 1.0 / 60.0);
    (void)state.pipeline.finish_frame(scene.root);

    scene.root.mark_dirty(Dirty::Paint | Dirty::Render);
    state.pipeline.tick(scene.root, 1.0 / 60.0);
    const FrameRender changed = state.pipeline.finish_frame(scene.root);
    bool ok = expect(changed.glass_dependencies == 0, "a non-glass tree reports no dependency");
    ok = expect(changed.damage.rects == 1, "a non-glass tree keeps one damage rect") && ok;
    ok = expect(changed.should_render, "a non-glass tree still renders changes") && ok;
    return ok;
}

bool paint_emits_glass() {
    Scene scene(true);
    Pipeline state;
    FrameTick tick = state.pipeline.tick(scene.root, 1.0 / 60.0);
    state.pipeline.sync_style(scene.root, scene.styles, tick, true);
    const FrameRender render = state.pipeline.finish_frame(scene.root);
    struct CountingSink final : PaintSink {
        void fill_rect(const tessera::rendering::rhi::Rect&,
                       const tessera::rendering::rhi::Color&) override {}
        void fill_rounded_rect(const tessera::rendering::rhi::Rect&,
                               const tessera::rendering::rhi::Color&, float) override {}
    } sink;
    const FramePaint paint = state.pipeline.paint_frame(scene.root, scene.styles, sink);
    bool ok = expect(render.should_render, "the frame is due");
    ok = expect(paint.painted, "the pipeline paints the frame") && ok;
    ok = expect(paint.stats.backdrops == 1, "the pipeline reports one glass backdrop") && ok;
    return ok;
}

bool diagnostics() {
    FrameDiagnostics diagnostics;
    FrameDiagnosticsSample sample;
    sample.frame_index = 7;
    sample.glass_ms = 1.5;
    sample.glass_backdrop_count = 2;
    diagnostics.record(sample);
    sample.glass_ms = 0.0;
    sample.glass_backdrop_count = 0;
    diagnostics.record(sample);
    bool ok = expect(diagnostics.frames() == 2, "two frames recorded");
    ok = expect(diagnostics.total_glass_ms() == 1.5, "glass_ms accumulates") && ok;
    ok = expect(diagnostics.total_glass_backdrops() == 2, "glass_backdrop_count accumulates") && ok;
    ok = expect(diagnostics.glass_frames() == 1, "only frames with glass work are counted") && ok;
    ok = expect(diagnostics.last().glass_backdrop_count == 0, "the latest sample is kept") && ok;
    diagnostics.reset();
    ok = expect(diagnostics.total_glass_ms() == 0.0 && diagnostics.total_glass_backdrops() == 0 &&
                    diagnostics.glass_frames() == 0,
                "reset clears the glass counters") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = style_sync();
    ok = dependency_region() && ok;
    ok = tracker_adds_glass_boxes() && ok;
    ok = frame_pipeline_idle_and_damage() && ok;
    ok = non_glass_damage_is_unchanged() && ok;
    ok = paint_emits_glass() && ok;
    ok = diagnostics() && ok;
    if (!ok) {
        std::cerr << "glass_damage: FAILED\n";
        return 1;
    }
    std::cout << "glass_damage: ok\n";
    return 0;
}
