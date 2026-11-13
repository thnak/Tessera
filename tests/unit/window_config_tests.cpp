// SPDX-License-Identifier: MIT
//
// The window driver's runtime-config mapping (docs/specs/runtime-config-v1.md section 6.2). This
// slice wires three groups of keys:
//   - `animationInterpolation` -> FramePipelineConfig::interpolation;
//   - `pauseWhenOccluded`      -> WindowPausePolicy for occluded and minimized windows;
//   - the adaptive-quality keys -> AdaptiveQualityPolicy (the controller itself is tested in
//     adaptive_quality_tests.cpp).
//
// The mapping layer is pure, so it is tested without a GPU or a window. The last case drives the
// mapped FramePipelineConfig through FramePipeline's paint seam: with the key off the painted
// transform is the authoritative tick state, with it on the frame paints the blended value.

#include "tessera/runtime/config.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/frame_pipeline.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "window_config.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace {

using tessera::PowerPolicy;
using tessera::RuntimeConfig;
using tessera::app::AdaptiveQualityPolicy;
using tessera::app::window_adaptive_quality_policy;
using tessera::app::window_pause_policy;
using tessera::app::window_pipeline_config;
using tessera::app::WindowPausePolicy;
using tessera::rendering::DamageAccumulator;
using tessera::rendering::DamageConfig;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Mat3;
using tessera::rendering::rhi::Rect;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Dirty;
using tessera::ui::DirtyPass;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::animation::AnimationInput;
using tessera::ui::animation::AnimationRuntime;
using tessera::ui::animation::KeyframeTiming;
using tessera::ui::frame::FramePipeline;
using tessera::ui::frame::FramePipelineConfig;
using tessera::ui::layout::DamageTracker;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::StyleProvider;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::KeyframeBlock;
using tessera::ui::style::KeyframeStep;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "window_config: " << message << "\n";
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

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color&) override {
        rects.push_back(rect);
    }

    void fill_rounded_rect(const Rect& rect, const Color&, float) override {
        rects.push_back(rect);
    }

    std::vector<Rect> rects;
};

// Every field window_pipeline_config maps is asserted by name: an accidental default flip in
// FramePipelineConfig must fail here instead of silently changing every existing caller.
bool pipeline_config_equals_default(const FramePipelineConfig& config) {
    bool ok = expect(config.dpi_scale == 1.0f, "default dpi scale is 1");
    ok = expect(config.damage_margin == 0.0f, "default damage margin is 0") && ok;
    ok = expect(config.redraw_policy.full_redraw_area_ratio == 0.6f,
                "default full-redraw area ratio is 0.6") &&
         ok;
    ok = expect(config.redraw_policy.max_scissors == 8, "default max scissors is 8") && ok;
    ok = expect(config.interpolation, "default interpolation is on") && ok;
    ok = expect(config.paint_cache, "default paint cache is on") && ok;
    return ok;
}

bool defaults_match_runtime_config_defaults() {
    const RuntimeConfig defaults{};
    const FramePipelineConfig pipeline = window_pipeline_config(defaults);
    const WindowPausePolicy policy = window_pause_policy(defaults);

    bool ok = pipeline_config_equals_default(pipeline);
    ok = expect(policy.pause_when_occluded, "default pause-when-occluded is on") && ok;
    ok = expect(policy.pause_when_minimized, "default pause-when-minimized is on") && ok;
    ok = expect(policy == WindowPausePolicy{},
                "the mapped default policy equals WindowPausePolicy{}") &&
         ok;
    return ok;
}

bool interpolation_key_maps_to_the_pipeline_config() {
    const FramePipelineConfig enabled = window_pipeline_config(RuntimeConfig{});
    RuntimeConfig disabled_runtime;
    disabled_runtime.animation_interpolation = false;
    const FramePipelineConfig disabled = window_pipeline_config(disabled_runtime);
    RuntimeConfig explicit_enabled_runtime;
    explicit_enabled_runtime.animation_interpolation = true;

    bool ok = expect(enabled.interpolation, "animationInterpolation=true keeps interpolation on");
    ok = expect(!disabled.interpolation, "animationInterpolation=false disables it") && ok;
    ok = expect(window_pipeline_config(explicit_enabled_runtime).interpolation,
                "an explicit true is on") &&
         ok;
    RuntimeConfig epsilon_runtime;
    epsilon_runtime.interpolation_epsilon_px = 0.5f;
    ok = expect(enabled.interpolation_epsilon_px == RuntimeConfig{}.interpolation_epsilon_px &&
                    window_pipeline_config(epsilon_runtime).interpolation_epsilon_px == 0.5f,
                "interpolationEpsilonPx maps to the pipeline's suppression distance") &&
         ok;
    // The key must not move any other mapped field.
    ok = expect(disabled.dpi_scale == enabled.dpi_scale &&
                    disabled.damage_margin == enabled.damage_margin &&
                    disabled.paint_cache == enabled.paint_cache &&
                    disabled.redraw_policy.full_redraw_area_ratio ==
                        enabled.redraw_policy.full_redraw_area_ratio &&
                    disabled.redraw_policy.max_scissors == enabled.redraw_policy.max_scissors,
                "the interpolation key changes interpolation only") &&
         ok;
    return ok;
}

bool pause_key_maps_to_the_lifecycle_policy() {
    const WindowPausePolicy paused = window_pause_policy(RuntimeConfig{});
    RuntimeConfig keep_rendering_runtime;
    keep_rendering_runtime.pause_when_occluded = false;
    const WindowPausePolicy keep_rendering = window_pause_policy(keep_rendering_runtime);

    bool ok = expect(paused.pause_when_occluded, "pauseWhenOccluded=true pauses occlusion");
    ok = expect(paused.pause_when_minimized, "pauseWhenOccluded=true pauses minimization") && ok;
    ok = expect(paused.pauses(true, false) && paused.pauses(false, true) &&
                    paused.pauses(true, true) && !paused.pauses(false, false),
                "the default policy pauses every invisible state and nothing else") &&
         ok;
    // `false` keeps rendering for both lifecycle sources; focus/blur is not a policy input at all.
    ok = expect(!keep_rendering.pause_when_occluded,
                "pauseWhenOccluded=false keeps rendering while occluded") &&
         ok;
    ok = expect(!keep_rendering.pause_when_minimized,
                "pauseWhenOccluded=false keeps rendering while minimized") &&
         ok;
    ok = expect(!keep_rendering.pauses(true, false) && !keep_rendering.pauses(false, true) &&
                    !keep_rendering.pauses(true, true),
                "the disabled policy never pauses") &&
         ok;
    return ok;
}

// --- Paint-seam proof through the mapped config ----------------------------------------------

KeyframeBlock slide_block() {
    KeyframeBlock block;
    block.name = "slide";
    // The keyframe sampler steps a transform list at the halfway point, so the two ticks carry
    // translateX(0) and translateX(40). The caller syncs the scene transform to the second tick's
    // value before that tick, exactly as AppHost does after style resolution: the authoritative
    // tick geometry and the interpolated paint position are then different by construction.
    block.steps.push_back(
        KeyframeStep{0.0, {Declaration{"transform", "translateX(0px)", false, 0}}});
    block.steps.push_back(
        KeyframeStep{100.0, {Declaration{"transform", "translateX(40px)", false, 0}}});
    return block;
}

FramePipelineConfig config_for(bool interpolation) {
    RuntimeConfig runtime;
    runtime.animation_interpolation = interpolation;
    return window_pipeline_config(runtime);
}

// The painted x of the moving box after two ticks painted at alpha 0.5.
float painted_box_x(bool interpolation) {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    Node& box = root.add_child(std::make_unique<Node>(2));
    box.set_bounds({10, 10, 20, 20});
    root.clear_dirty(Dirty::All);
    box.clear_dirty(Dirty::All);

    StubStyles styles;
    ComputedStyle background;
    background.set("background-color", "blue");
    styles.set(2, std::move(background));

    ManualClock clock;
    RuntimeLoop loop(clock);
    AnimationRuntime animations;
    DamageAccumulator damage(DamageConfig{Extent2D{200, 200}, 8});
    DamageTracker tracker;
    DirtyPass dirty;
    const FramePipelineConfig config = config_for(interpolation);
    FramePipeline pipeline(animations, loop, tracker, damage, dirty, config);

    KeyframeTiming timing;
    timing.duration = 0.2;
    animations.start(AnimationInput{2, slide_block(), timing, {}, "", false});
    (void)pipeline.tick(root, 0.05, 0.5); // samples translateX(0)
    (void)pipeline.finish_frame(root);

    box.set_transform(Mat3::translation(40.0f, 0.0f)); // the authoritative second-tick state
    root.clear_dirty(Dirty::All);
    box.clear_dirty(Dirty::All);
    (void)pipeline.tick(root, 0.1, 0.5); // samples translateX(40)
    (void)pipeline.finish_frame(root);

    RecordingSink sink;
    (void)pipeline.paint_frame(root, styles, sink, true);
    return sink.rects.empty() ? -1.0f : sink.rects.back().x;
}

bool mapped_interpolation_reaches_the_paint_seam() {
    const float disabled = painted_box_x(false);
    const float enabled = painted_box_x(true);
    bool ok = expect(disabled == 50.0f,
                     "animationInterpolation=false paints the authoritative tick transform");
    ok = expect(enabled == 30.0f, "animationInterpolation=true paints the blended transform") && ok;
    return ok;
}

// --- Adaptive quality policy ----------------------------------------------------------------

bool adaptive_quality_keys_map_to_the_policy() {
    const AdaptiveQualityPolicy defaults = window_adaptive_quality_policy(RuntimeConfig{});
    bool ok = expect(defaults == AdaptiveQualityPolicy{},
                     "the mapped default policy equals AdaptiveQualityPolicy{}");
    ok = expect(defaults.adaptive_quality, "default adaptiveQuality is on") && ok;
    ok =
        expect(defaults.power_policy == PowerPolicy::Adaptive, "default powerPolicy is adaptive") &&
        ok;
    ok = expect(defaults.render_scale == 1.0f, "default renderScale is 1") && ok;
    ok = expect(defaults.min_render_scale == 0.6f, "default minRenderScale is 0.6") && ok;
    ok = expect(defaults.frame_budget_ms == 0.0, "default frameBudgetMs is 0 (derive)") && ok;
    ok =
        expect(defaults.target_fps == 60.0, "an unknown rate uses the documented 60 Hz baseline") &&
        ok;

    RuntimeConfig runtime;
    runtime.adaptive_quality = false;
    runtime.power_policy = PowerPolicy::Benchmark;
    runtime.render_scale = 2.0f;
    runtime.min_render_scale = 0.75f;
    runtime.frame_budget_ms = 8.5;
    const AdaptiveQualityPolicy explicit_policy = window_adaptive_quality_policy(runtime);
    ok = expect(!explicit_policy.adaptive_quality, "adaptiveQuality=false is mapped") && ok;
    ok = expect(explicit_policy.power_policy == PowerPolicy::Benchmark,
                "powerPolicy is mapped verbatim") &&
         ok;
    ok = expect(explicit_policy.render_scale == 2.0f && explicit_policy.min_render_scale == 0.75f,
                "the scale pair is mapped verbatim") &&
         ok;
    ok = expect(explicit_policy.frame_budget_ms == 8.5, "frameBudgetMs is mapped verbatim") && ok;

    // The derived rate: a present cap wins over a requested refresh, which wins over the baseline.
    RuntimeConfig capped;
    capped.max_fps = 30;
    capped.preferred_refresh_rate = 144;
    ok = expect(window_adaptive_quality_policy(capped).target_fps == 30.0,
                "maxFps derives the target rate") &&
         ok;
    RuntimeConfig preferred;
    preferred.preferred_refresh_rate = 144;
    ok = expect(window_adaptive_quality_policy(preferred).target_fps == 144.0,
                "preferredRefreshRate derives the target rate when maxFps is 0") &&
         ok;

    // The policy must not move any other mapped field.
    const FramePipelineConfig enabled_pipeline = window_pipeline_config(RuntimeConfig{});
    const FramePipelineConfig other_keys = window_pipeline_config(runtime);
    ok = expect(enabled_pipeline.interpolation == other_keys.interpolation &&
                    enabled_pipeline.paint_cache == other_keys.paint_cache,
                "the adaptive-quality keys change no pipeline field") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = defaults_match_runtime_config_defaults() && ok;
    ok = interpolation_key_maps_to_the_pipeline_config() && ok;
    ok = pause_key_maps_to_the_lifecycle_policy() && ok;
    ok = adaptive_quality_keys_map_to_the_policy() && ok;
    ok = mapped_interpolation_reaches_the_paint_seam() && ok;
    if (!ok) {
        std::cerr << "window_config: FAILED\n";
        return 1;
    }
    std::cout << "window_config: ok\n";
    return 0;
}
