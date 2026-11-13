// SPDX-License-Identifier: MIT
//
// Adaptive quality, deterministically (docs/design/performance.md section 6; the keys are
// `adaptiveQuality`, `powerPolicy`, `renderScale`, `minRenderScale` and `frameBudgetMs` in
// docs/specs/runtime-config-v1.md sections 3, 5.6, 5.7 and 6.2).
//
// The controller is a pure state machine: these cases fix its thresholds, its clamps, its disabled
// paths, its hysteresis and its determinism. The last case drives a real AppHost frame loop with a
// ManualClock (never a wall clock) and proves that one observation per frame reaches the host and
// that a change - not the raw frame time - is what lands in the frame diagnostics. It also proves
// that the decision leaves the host's own geometry alone: the render-target consumer is the window
// driver (tests/gpu/app_window_render_scale_tests.cpp), so layout and damage stay independent.

#include "adaptive_quality.hpp"
#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/diagnostics.hpp"
#include "tessera/runtime/runtime_loop.hpp"
#include "tessera/ui/paint/paint_sink.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using tessera::PowerPolicy;
using tessera::app::AdaptiveQualityPolicy;
using tessera::app::AppHost;
using tessera::app::HostConfig;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Extent2D;
using tessera::rendering::rhi::Rect;
using tessera::runtime::AdaptiveQualityConfig;
using tessera::runtime::AdaptiveQualityController;
using tessera::runtime::AdaptiveQualitySample;
using tessera::runtime::FrameDiagnostics;
using tessera::runtime::kAdaptiveQualityCooldownFrames;
using tessera::runtime::kAdaptiveQualityDowngradeMisses;
using tessera::runtime::kAdaptiveQualityEffectSteps;
using tessera::runtime::kAdaptiveQualityLadderSteps;
using tessera::runtime::kAdaptiveQualitySteps;
using tessera::runtime::kAdaptiveQualityUpgradeHits;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::ui::Node;
using tessera::ui::style::compile_stylesheet;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "adaptive_quality: " << message << "\n";
    }
    return condition;
}

// One over-budget observation and one inside the budget, both far enough from the 16 ms boundary
// that no floating-point noise can decide the comparison.
constexpr double kMissMs = 20.0;
constexpr double kMetMs = 10.0;

// render_scale=1.0 and min_render_scale=0.5 make every ladder step exactly representable
// (1.0, 0.875, 0.75, 0.625, 0.5), so the tests compare floats exactly instead of with an epsilon.
AdaptiveQualityConfig base_config() {
    AdaptiveQualityConfig config;
    config.adaptive_quality = true;
    config.power_policy = PowerPolicy::Adaptive;
    config.render_scale = 1.0f;
    config.min_render_scale = 0.5f;
    config.frame_budget_ms = 16.0;
    config.target_fps = 60.0;
    return config;
}

AdaptiveQualityConfig capped_config() {
    AdaptiveQualityConfig config = base_config();
    config.max_fps = 0.0;
    config.refresh_hz = 60.0;
    return config;
}

bool same_sample(const AdaptiveQualitySample& lhs, const AdaptiveQualitySample& rhs) {
    return lhs.render_scale == rhs.render_scale && lhs.quality_step == rhs.quality_step &&
           lhs.changed == rhs.changed && lhs.downgrade == rhs.downgrade &&
           lhs.present_cap_hz == rhs.present_cap_hz;
}

// --- 1. Thresholds and cooldown ---------------------------------------------------------------

bool miss_run_downgrades_at_the_threshold() {
    AdaptiveQualityController controller(base_config());
    bool ok = true;
    // "Not one frame earlier": the first threshold-1 misses change nothing.
    for (std::uint32_t frame = 0; frame + 1 < kAdaptiveQualityDowngradeMisses; ++frame) {
        const AdaptiveQualitySample sample = controller.observe(kMissMs);
        ok = expect(!sample.changed && sample.quality_step == 0,
                    "a downgrade waits for the whole miss run") &&
             ok;
        ok = expect(!sample.downgrade, "an unchanged frame reports no direction") && ok;
    }
    const AdaptiveQualitySample change = controller.observe(kMissMs);
    ok = expect(change.changed, "the threshold frame downgrades") && ok;
    ok = expect(change.downgrade, "a downgrade is reported as one") && ok;
    ok = expect(change.quality_step == 1, "a downgrade moves exactly one ladder step") && ok;
    // Position 1 is the effects stage: it reduces optional effects and leaves the scale alone.
    ok = expect(change.effects_reduced && !change.animations_paused,
                "the first ladder position reduces optional effects only") &&
         ok;
    ok = expect(change.render_scale == 1.0f, "the effects stage does not touch renderScale") && ok;
    ok = expect(controller.cooldown_remaining() == kAdaptiveQualityCooldownFrames,
                "a change arms the cooldown") &&
         ok;
    return ok;
}

bool cooldown_suppresses_a_second_change() {
    AdaptiveQualityController controller(base_config());
    bool ok = true;
    for (std::uint32_t frame = 0; frame < kAdaptiveQualityDowngradeMisses; ++frame) {
        (void)controller.observe(kMissMs);
    }
    ok = expect(controller.decision().quality_step == 1, "the miss run downgraded once") && ok;
    // The cooldown frames are not evidence for the next run: a sustained overload cannot step down
    // once per cooldown frame, it waits out the cooldown and then the whole miss run again.
    for (std::uint32_t frame = 0; frame < kAdaptiveQualityCooldownFrames; ++frame) {
        const AdaptiveQualitySample sample = controller.observe(kMissMs);
        ok = expect(!sample.changed, "the cooldown suppresses a second change") && ok;
    }
    for (std::uint32_t frame = 0; frame + 1 < kAdaptiveQualityDowngradeMisses; ++frame) {
        const AdaptiveQualitySample sample = controller.observe(kMissMs);
        ok = expect(!sample.changed, "the next run restarts from zero after the cooldown") && ok;
    }
    const AdaptiveQualitySample second = controller.observe(kMissMs);
    ok = expect(second.changed && second.quality_step == 2,
                "the second downgrade lands one full run after the cooldown") &&
         ok;
    return ok;
}

bool met_run_upgrades_at_the_threshold() {
    AdaptiveQualityController controller(base_config());
    bool ok = true;
    // Get to step 1 first: an upgrade needs somewhere to go.
    for (std::uint32_t frame = 0; frame < kAdaptiveQualityDowngradeMisses; ++frame) {
        (void)controller.observe(kMissMs);
    }
    ok = expect(controller.decision().quality_step == 1, "the setup frame downgraded once") && ok;
    for (std::uint32_t frame = 0; frame < kAdaptiveQualityCooldownFrames; ++frame) {
        (void)controller.observe(kMetMs);
    }
    for (std::uint32_t frame = 0; frame + 1 < kAdaptiveQualityUpgradeHits; ++frame) {
        const AdaptiveQualitySample sample = controller.observe(kMetMs);
        ok = expect(!sample.changed, "an upgrade waits for the whole met run") && ok;
    }
    const AdaptiveQualitySample upgrade = controller.observe(kMetMs);
    ok = expect(upgrade.changed, "the threshold frame upgrades") && ok;
    ok = expect(!upgrade.downgrade, "an upgrade is not a downgrade") && ok;
    ok = expect(upgrade.quality_step == 0 && upgrade.render_scale == 1.0f,
                "an upgrade returns to the configured render scale") &&
         ok;
    return ok;
}

// --- 2. Bounds --------------------------------------------------------------------------------

bool misses_stop_at_the_floor() {
    AdaptiveQualityConfig config = base_config();
    config.frame_budget_ms = 16.0;
    AdaptiveQualityController controller(config);
    std::uint32_t changes = 0;
    bool in_range = true;
    for (int frame = 0; frame < 400; ++frame) {
        const AdaptiveQualitySample sample = controller.observe(kMissMs);
        changes += sample.changed ? 1 : 0;
        in_range = in_range && sample.render_scale >= config.min_render_scale &&
                   sample.render_scale <= config.render_scale;
    }
    bool ok = expect(in_range, "every decision stays inside [minRenderScale, renderScale]");
    ok = expect(changes == kAdaptiveQualityLadderSteps,
                "a sustained overload spends the whole ladder and then stops changing") &&
         ok;
    ok = expect(controller.decision().quality_step == kAdaptiveQualityLadderSteps,
                "the ladder index stops at the last step") &&
         ok;
    ok = expect(controller.decision().effects_reduced && controller.decision().animations_paused,
                "the last position keeps effects reduced and pauses animations") &&
         ok;
    ok = expect(controller.decision().render_scale == config.min_render_scale,
                "the last step is exactly minRenderScale, not an interpolation of it") &&
         ok;
    return ok;
}

bool met_frames_stop_at_the_ceiling() {
    // A configured renderScale above 1.0: the ceiling is the configured value, not 1.0.
    AdaptiveQualityConfig config = base_config();
    config.render_scale = 2.0f;
    config.min_render_scale = 0.6f;
    AdaptiveQualityController controller(config);
    bool ok = true;
    for (int frame = 0; frame < 200; ++frame) {
        const AdaptiveQualitySample sample = controller.observe(kMetMs);
        ok = expect(!sample.changed, "full quality never upgrades past renderScale") && ok;
    }
    ok = expect(controller.decision().render_scale == config.render_scale,
                "the decision stops exactly at the configured renderScale") &&
         ok;
    ok = expect(controller.decision().quality_step == 0, "full quality is step zero") && ok;

    // Drive it to the floor and check the lower clamp for a non-unit ceiling.
    for (int frame = 0; frame < 400; ++frame) {
        (void)controller.observe(kMissMs);
    }
    ok = expect(controller.decision().render_scale == config.min_render_scale,
                "the floor holds for a non-unit renderScale") &&
         ok;
    return ok;
}

bool equal_bounds_never_change() {
    AdaptiveQualityConfig config = base_config();
    config.min_render_scale = config.render_scale;
    AdaptiveQualityController controller(config);
    bool ok = expect(!controller.active(), "equal min/render scales leave no room to adapt");
    for (int frame = 0; frame < 200; ++frame) {
        const AdaptiveQualitySample sample = controller.observe(kMissMs);
        ok = expect(!sample.changed, "an empty ladder never changes") && ok;
    }
    ok = expect(controller.decision().render_scale == config.render_scale,
                "an empty ladder keeps the configured scale") &&
         ok;
    return ok;
}

// --- 3. Disabled paths ------------------------------------------------------------------------

bool disabled_paths_never_change() {
    bool ok = true;
    AdaptiveQualityConfig off = capped_config();
    off.max_fps = 60.0;
    off.adaptive_quality = false;
    AdaptiveQualityController off_controller(off);
    ok = expect(!off_controller.active(), "adaptiveQuality=false deactivates the controller") && ok;

    AdaptiveQualityConfig benchmark = capped_config();
    benchmark.max_fps = 60.0;
    benchmark.power_policy = PowerPolicy::Benchmark;
    AdaptiveQualityController benchmark_controller(benchmark);
    ok = expect(!benchmark_controller.active(),
                "powerPolicy=benchmark deactivates the controller") &&
         ok;

    for (int frame = 0; frame < 200; ++frame) {
        const AdaptiveQualitySample off_sample = off_controller.observe(kMissMs);
        const AdaptiveQualitySample benchmark_sample = benchmark_controller.observe(kMissMs);
        ok = expect(!off_sample.changed && off_sample.quality_step == 0,
                    "a disabled controller never changes") &&
             ok;
        ok = expect(off_sample.present_cap_hz == off.max_fps,
                    "a disabled controller never changes the present cap") &&
             ok;
        ok = expect(off_sample.render_scale == off.render_scale,
                    "a disabled controller stays at the configured scale") &&
             ok;
        ok = expect(!benchmark_sample.changed && benchmark_sample.quality_step == 0,
                    "benchmark never changes quality") &&
             ok;
    }
    return ok;
}

// --- 4. No oscillation ------------------------------------------------------------------------

bool alternating_frames_never_change() {
    AdaptiveQualityController controller(base_config());
    std::uint32_t changes = 0;
    // 200 frames of miss/met alternation: no run ever reaches either threshold.
    for (int frame = 0; frame < 200; ++frame) {
        changes += controller.observe(frame % 2 == 0 ? kMissMs : kMetMs).changed ? 1 : 0;
    }
    bool ok = expect(changes == 0, "an alternating miss/met sequence produces exactly no changes");
    ok = expect(controller.decision().quality_step == 0, "alternation leaves full quality") && ok;

    // Three-miss bursts separated by a single met frame: still one short of the downgrade run.
    std::uint32_t burst_changes = 0;
    for (int frame = 0; frame < 200; ++frame) {
        const bool met = frame % 4 == 3;
        burst_changes += controller.observe(met ? kMetMs : kMissMs).changed ? 1 : 0;
    }
    ok = expect(burst_changes == 0, "a sub-threshold burst never changes") && ok;
    ok = expect(controller.cooldown_remaining() == 0, "nothing armed a cooldown") && ok;
    return ok;
}

// --- 5. Determinism ---------------------------------------------------------------------------

// A deterministic script: alternating stretches, calm stretches and sustained overload, no RNG.
double scripted_frame_time(std::uint32_t frame) {
    if (frame % 37 < 9) {
        return kMissMs;
    }
    if (frame % 11 == 0) {
        return 17.5; // just over the 16 ms budget
    }
    return kMetMs;
}

std::vector<AdaptiveQualitySample> run_scripted_sequence() {
    AdaptiveQualityController controller(base_config());
    std::vector<AdaptiveQualitySample> samples;
    samples.reserve(500);
    for (std::uint32_t frame = 0; frame < 500; ++frame) {
        samples.push_back(controller.observe(scripted_frame_time(frame)));
    }
    return samples;
}

bool identical_sequences_decide_identically() {
    // No owned storage and no virtual functions: copying the controller state is all it takes to
    // reproduce a sequence, and nothing allocates on the observation path.
    static_assert(std::is_trivially_copyable_v<AdaptiveQualityController>,
                  "the controller must hold only scalars so a copy reproduces its decision state");
    static_assert(std::is_trivially_destructible_v<AdaptiveQualityController>,
                  "the controller must not own a resource");
    static_assert(std::is_trivially_copyable_v<AdaptiveQualityConfig>);
    static_assert(std::is_trivially_copyable_v<AdaptiveQualitySample>);

    const std::vector<AdaptiveQualitySample> first = run_scripted_sequence();
    const std::vector<AdaptiveQualitySample> second = run_scripted_sequence();
    bool ok = expect(first.size() == second.size(), "both runs observed the same frames");
    for (std::size_t index = 0; ok && index < first.size(); ++index) {
        if (!expect(same_sample(first[index], second[index]),
                    "identical inputs decide identically")) {
            ok = false;
        }
    }

    // A copy taken mid-sequence continues identically: the state machine has no hidden state.
    AdaptiveQualityController original(base_config());
    for (std::uint32_t frame = 0; frame < 40; ++frame) {
        (void)original.observe(scripted_frame_time(frame));
    }
    AdaptiveQualityController copy = original;
    for (std::uint32_t frame = 40; frame < 200; ++frame) {
        const AdaptiveQualitySample lhs = original.observe(scripted_frame_time(frame));
        const AdaptiveQualitySample rhs = copy.observe(scripted_frame_time(frame));
        if (!expect(same_sample(lhs, rhs), "a copied controller continues the same sequence")) {
            ok = false;
            break;
        }
    }
    return ok;
}

// --- 6. The present-cap component (step 3) ------------------------------------------------------

bool the_decision_carries_the_present_cap() {
    bool ok = true;
    AdaptiveQualityController controller(capped_config());
    ok = expect(controller.decision().present_cap_hz == 0.0,
                "step 0 keeps the configured cap (uncapped)") &&
         ok;
    for (std::uint32_t frame = 0; frame < kAdaptiveQualityDowngradeMisses; ++frame) {
        (void)controller.observe(kMissMs);
    }
    const AdaptiveQualitySample downgraded = controller.decision();
    ok = expect(downgraded.downgrade && downgraded.quality_step == 1 &&
                    downgraded.present_cap_hz == 0.0,
                "the effects stage leaves the cap alone") &&
         ok;
    for (std::uint32_t frame = 0;
         frame < kAdaptiveQualityCooldownFrames + kAdaptiveQualityDowngradeMisses; ++frame) {
        (void)controller.observe(kMissMs);
    }
    ok = expect(controller.decision().quality_step == 2 &&
                    controller.decision().present_cap_hz == 30.0,
                "the cap moves to refresh/2 with the first scale step") &&
         ok;
    // The rule is a pure function of the step: a consumer can ask without driving the machine.
    ok = expect(controller.present_cap_for_step(0) == 0.0, "step 0 asks for the configured cap") &&
         ok;
    ok = expect(controller.present_cap_for_step(2) == 20.0, "step 2 asks for refresh/3") && ok;
    return ok;
}

// --- 7. Wiring: the host observes frames and diagnostics record the change ---------------------

class RecordingSink final : public tessera::ui::paint::PaintSink {
public:
    void fill_rect(const Rect& rect, const Color&) override {
        rects.push_back(rect);
    }
    void fill_rounded_rect(const Rect& rect, const Color&, float) override {
        rects.push_back(rect);
    }
    std::vector<Rect> rects;
};

bool host_observes_frames_and_records_the_change() {
    Node root(1);
    Node& card = root.add_child(std::make_unique<Node>(2));
    card.set_class_name("card");
    auto sheet = compile_stylesheet(R"(
.card { width: 100px; height: 40px; background-color: #1e293b; }
)");

    ManualClock clock;
    RuntimeLoop loop(clock);
    HostConfig config;
    config.width = 320.0f;
    config.height = 200.0f;
    // An explicit budget, so the test is not coupled to the mapping layer's derived rate; the
    // ladder values stay exactly representable (1.0, 0.875, 0.75, 0.625, 0.5).
    config.adaptive_quality = AdaptiveQualityPolicy{};
    config.adaptive_quality.frame_budget_ms = 16.0;
    config.adaptive_quality.render_scale = 1.0f;
    config.adaptive_quality.min_render_scale = 0.5f;
    AppHost host(root, std::move(sheet), loop, config);

    const Extent2D extent_before = host.target_extent();
    bool ok = expect(host.adaptive_quality().quality_step == 0, "the host starts at full quality");
    ok = expect(host.frame_diagnostics().adaptive_quality_changes() == 0,
                "no change is recorded before any observation") &&
         ok;

    // One driver frame: advance the ManualClock, tick the loop, run the host's frame and feed the
    // measured time - the same order WindowHost::run uses, without a wall clock.
    const auto run_frame = [&](double frame_ms) {
        clock.advance(1.0 / 60.0);
        loop.mark_render_dirty();
        const tessera::runtime::RuntimeTick tick = loop.tick();
        (void)host.begin_frame(1.0 / 60.0, tick.alpha);
        host.end_frame();
        host.observe_frame_time_ms(frame_ms);
    };

    for (std::uint32_t frame = 0; frame + 1 < kAdaptiveQualityDowngradeMisses; ++frame) {
        run_frame(kMissMs);
        ok = expect(host.frame_diagnostics().adaptive_quality_changes() == 0,
                    "sub-threshold frames record no change") &&
             ok;
    }

    run_frame(kMissMs);
    ok = expect(host.adaptive_quality().changed, "the threshold frame is the one the host reports");
    ok = expect(host.adaptive_quality().quality_step == 1 &&
                    host.adaptive_quality().effects_reduced &&
                    host.adaptive_quality().render_scale == 1.0f,
                "the host's decision is the controller's ladder position") &&
         ok;
    ok = expect(host.frame_diagnostics().adaptive_effect_changes() == 1 &&
                    host.frame_diagnostics().adaptive_animation_pause_changes() == 0,
                "the effects flip is counted, the pause flag has not moved") &&
         ok;
    ok = expect(host.frame_diagnostics().adaptive_quality_changes() == 1,
                "the change is what the frame diagnostics count") &&
         ok;
    ok = expect(host.frame_diagnostics().adaptive_quality_downgrades() == 1,
                "the change is recorded as a downgrade") &&
         ok;

    // The host's own render geometry is unchanged: the damage target, the layout and the paint
    // commands stay in the window's presented pixel space. The render-target consumer is the window
    // driver (WindowHost), which renders those same commands into a smaller target and resamples it
    // up; see tests/gpu/app_window_render_scale_tests.cpp. Keeping this state independent is what
    // lets the adaptive multiplier change without touching layout or the glyph atlas.
    const Extent2D extent_after = host.target_extent();
    ok = expect(extent_after.width == extent_before.width &&
                    extent_after.height == extent_before.height,
                "a downgrade does not resize the host's damage target") &&
         ok;
    host.request_full_repaint();
    run_frame(kMetMs);
    RecordingSink sink;
    const auto paint = host.paint(sink);
    ok = expect(paint.painted, "the frame after a downgrade still paints normally") && ok;
    ok = expect(!sink.rects.empty(), "paint output is unaffected by the decision") && ok;

    // The next frame is inside the cooldown: the observation is still recorded (latest decision)
    // but the change counters stay put - only a change is news.
    run_frame(kMissMs);
    ok = expect(!host.adaptive_quality().changed, "a cooldown frame reports no change") && ok;
    ok = expect(host.frame_diagnostics().adaptive_quality_changes() == 1,
                "a cooldown observation does not increment the change count") &&
         ok;
    ok = expect(host.frame_diagnostics().adaptive_quality().effects_reduced,
                "diagnostics keep the latest decision in force") &&
         ok;

    // A frame without a measurement is not an observation: the counters cannot move.
    host.observe_frame_time_ms(0.0);
    ok = expect(host.frame_diagnostics().adaptive_quality_changes() == 1,
                "a missing frame time is not an observation") &&
         ok;
    return ok;
}

// --- 8. The ladder ends (ADR-019 section 8, #147) -----------------------------------------------

bool ladder_order_is_effects_scale_then_pause() {
    AdaptiveQualityController controller(base_config());
    bool ok = true;
    AdaptiveQualitySample previous = controller.decision();
    for (std::uint32_t position = 1; position <= kAdaptiveQualityLadderSteps; ++position) {
        for (std::uint32_t frame = 0;
             frame < kAdaptiveQualityCooldownFrames + kAdaptiveQualityDowngradeMisses; ++frame) {
            (void)controller.observe(kMissMs);
        }
        const AdaptiveQualitySample now = controller.decision();
        ok = expect(now.quality_step == position, "one ladder position per downgrade run") && ok;
        ok = expect(now.effects_reduced, "effects stay reduced at every later position") && ok;
        if (position == 1) {
            ok = expect(now.render_scale == previous.render_scale && !now.animations_paused,
                        "position 1 changes only the effects flag") &&
                 ok;
        } else if (position < kAdaptiveQualityLadderSteps) {
            ok = expect(now.render_scale < previous.render_scale && !now.animations_paused,
                        "the middle positions walk the scale and never pause animations") &&
                 ok;
        } else {
            ok = expect(now.render_scale == previous.render_scale && now.animations_paused,
                        "the last position changes only the pause flag") &&
                 ok;
        }
        previous = now;
    }
    return ok;
}

bool recovery_restores_pause_then_effects_in_reverse() {
    AdaptiveQualityController controller(base_config());
    for (std::uint32_t frame = 0; frame < 400; ++frame) {
        (void)controller.observe(kMissMs);
    }
    bool ok = expect(controller.decision().animations_paused, "setup reached the last position");
    const auto upgrade_once = [&controller] {
        for (std::uint32_t frame = 0;
             frame < kAdaptiveQualityCooldownFrames + kAdaptiveQualityUpgradeHits; ++frame) {
            (void)controller.observe(kMetMs);
        }
        return controller.decision();
    };
    AdaptiveQualitySample sample = upgrade_once();
    ok = expect(!sample.animations_paused && sample.effects_reduced,
                "the first recovery resumes animations before anything else") &&
         ok;
    for (std::uint32_t position = kAdaptiveQualityLadderSteps - 1; position > 1; --position) {
        sample = upgrade_once();
        ok = expect(sample.effects_reduced, "effects stay reduced while the scale recovers") && ok;
    }
    sample = upgrade_once();
    ok = expect(sample.quality_step == 0 && !sample.effects_reduced && sample.render_scale == 1.0f,
                "the last recovery restores the effects") &&
         ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = miss_run_downgrades_at_the_threshold() && ok;
    ok = cooldown_suppresses_a_second_change() && ok;
    ok = met_run_upgrades_at_the_threshold() && ok;
    ok = misses_stop_at_the_floor() && ok;
    ok = met_frames_stop_at_the_ceiling() && ok;
    ok = equal_bounds_never_change() && ok;
    ok = disabled_paths_never_change() && ok;
    ok = alternating_frames_never_change() && ok;
    ok = identical_sequences_decide_identically() && ok;
    ok = the_decision_carries_the_present_cap() && ok;
    ok = host_observes_frames_and_records_the_change() && ok;
    ok = ladder_order_is_effects_scale_then_pause() && ok;
    ok = recovery_restores_pause_then_effects_in_reverse() && ok;
    if (!ok) {
        std::cerr << "adaptive_quality: FAILED\n";
        return 1;
    }
    std::cout << "adaptive_quality: ok\n";
    return 0;
}
