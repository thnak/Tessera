// SPDX-License-Identifier: MIT
//
// The adaptive-quality stage of AppHost (god-type audit slice 10, section 3.4), tested without a
// host: a fake context records what the stage did, so the two application rules that used to be
// implicit in host.cpp's ordering are pinned here.
//
// The rules under test:
//   - the pure selection rule picks the GPU number when present and the CPU time otherwise, and the
//     diagnostics see which source ran;
//   - a decision that moves the ladder is published to the diagnostics;
//   - a reduced-effects flip reaches the pipeline exactly once per transition (including the
//     no-op case, which must not repeatedly "apply" the same value);
//   - a paused-animations flip reaches the animation runtime once per transition.
//
// A full ladder walk (eight misses before a step) is the controller's own test; here the frame time
// is deliberately extreme so a single observation moves it, keeping this file about the stage.

#include "quality_stage.hpp"
#include "tessera/runtime/diagnostics.hpp"
#include "tessera/runtime/frame_time_source.hpp"

#include <iostream>
#include <optional>

namespace {

using tessera::app::detail::QualityStage;
using tessera::app::detail::QualityStageContext;
using tessera::runtime::AdaptiveQualityConfig;
using tessera::runtime::AdaptiveQualitySample;
using tessera::runtime::FrameDiagnostics;
using tessera::runtime::FrameTimeSource;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "app_quality_stage: " << message << "\n";
    }
    return condition;
}

// The host state the stage's callbacks touch, faked.
struct Fake {
    bool blurred_shadow = false;
    bool animations_running = false;
    bool reduce_effects = false;
    bool animations_paused = false;
    // How many times a callback actually applied a value (not merely fired).
    unsigned reduce_effect_calls = 0;
    unsigned animation_pause_calls = 0;
    // The full-repaint / loop-wake requests the flip rules imply.
    unsigned repaint_requests = 0;
    unsigned render_dirty_marks = 0;
};

bool blurred_shadow_probe(const void* context) noexcept {
    return static_cast<const Fake*>(context)->blurred_shadow;
}

bool animations_probe(const void* context) noexcept {
    return static_cast<const Fake*>(context)->animations_running;
}

void set_reduce_effects(void* context, bool reduced) {
    Fake& fake = *static_cast<Fake*>(context);
    if (fake.reduce_effects == reduced) {
        return; // an unchanged value is not an application, and must not request a repaint
    }
    fake.reduce_effects = reduced;
    ++fake.reduce_effect_calls;
    ++fake.repaint_requests;
}

void set_animations_paused(void* context, bool paused) {
    Fake& fake = *static_cast<Fake*>(context);
    if (fake.animations_paused == paused) {
        return;
    }
    fake.animations_paused = paused;
    ++fake.animation_pause_calls;
    ++fake.render_dirty_marks;
}

struct Fixture {
    Fake fake;
    FrameDiagnostics diagnostics;
    QualityStage stage;

    // A budget of 16 ms, adaptive on, so a 16 ms frame is on time and a much larger one is a miss.
    explicit Fixture(AdaptiveQualityConfig config = {})
        : stage(QualityStageContext{&blurred_shadow_probe, &animations_probe, &set_reduce_effects,
                                    &set_animations_paused, &diagnostics, &fake},
                config) {}
};

AdaptiveQualityConfig enabled_config() {
    AdaptiveQualityConfig config;
    config.adaptive_quality = true;
    config.frame_budget_ms = 16.0;
    config.target_fps = 60.0;
    return config;
}

bool check_on_time_frames_do_not_change_anything() {
    Fixture fixture{enabled_config()};
    for (int i = 0; i < 4; ++i) {
        const AdaptiveQualitySample& decision = fixture.stage.observe(16.0, std::nullopt);
        if (!expect(!decision.changed, "an on-time frame changes nothing")) {
            return false;
        }
    }
    return expect(fixture.fake.reduce_effect_calls == 0,
                  "no repaint request from on-time frames") &&
           expect(fixture.fake.animation_pause_calls == 0,
                  "no animation flip from on-time frames") &&
           expect(fixture.diagnostics.frame_time_source() == FrameTimeSource::Cpu,
                  "the CPU source is recorded");
}

bool check_gpu_interval_wins() {
    Fixture fixture{enabled_config()};
    static_cast<void>(fixture.stage.observe(16.0, 5.0));
    return expect(fixture.diagnostics.frame_time_source() == FrameTimeSource::Gpu,
                  "a present GPU interval is the recorded source") &&
           expect(fixture.diagnostics.gpu_frame_time_samples() == 1, "the GPU sample is counted") &&
           expect(fixture.diagnostics.cpu_frame_time_samples() == 0,
                  "a GPU sample does not also count as a CPU sample");
}

bool check_missing_gpu_interval_is_a_fallback() {
    Fixture fixture{enabled_config()};
    static_cast<void>(fixture.stage.observe(16.0, std::nullopt));
    return expect(fixture.diagnostics.frame_time_source() == FrameTimeSource::Cpu,
                  "a missing interval falls back to the CPU time") &&
           expect(fixture.diagnostics.cpu_frame_time_samples() == 1,
                  "the fallback is counted as a CPU sample") &&
           expect(fixture.diagnostics.gpu_frame_time_samples() == 0, "and not as a GPU sample");
}

bool check_slow_frames_reduce_effects_and_request_one_repaint() {
    Fixture fixture{enabled_config()};
    fixture.fake.blurred_shadow = true; // the effect the first ladder position reduces
    // Drive the ladder with a clearly-over-budget frame until a change lands.
    bool saw_change = false;
    for (int i = 0; i < 64 && !saw_change; ++i) {
        const AdaptiveQualitySample& decision = fixture.stage.observe(200.0, std::nullopt);
        saw_change = decision.changed;
    }
    if (!expect(saw_change, "a sustained over-budget run moves the ladder")) {
        return false;
    }
    return expect(fixture.fake.reduce_effects, "the reduced-effects flag reached the pipeline") &&
           expect(fixture.fake.reduce_effect_calls == 1,
                  "the flip was applied once, not once per frame") &&
           expect(fixture.fake.repaint_requests == 1,
                  "a reduced-effects flip requests exactly one full repaint");
}

bool check_paused_animations_wake_the_loop() {
    Fixture fixture{enabled_config()};
    fixture.fake.animations_running = true;
    bool paused = false;
    for (int i = 0; i < 64 && !paused; ++i) {
        static_cast<void>(fixture.stage.observe(200.0, std::nullopt));
        paused = fixture.fake.animations_paused;
    }
    if (!expect(paused, "a sustained over-budget run pauses non-essential animations")) {
        return false;
    }
    return expect(fixture.fake.animation_pause_calls == 1, "the pause flip was applied once") &&
           expect(fixture.fake.render_dirty_marks == 1,
                  "pausing marks the loop dirty exactly once");
}

bool check_disabled_ladder_never_moves() {
    AdaptiveQualityConfig config = enabled_config();
    config.adaptive_quality = false;
    Fixture fixture{config};
    fixture.fake.blurred_shadow = true;
    fixture.fake.animations_running = true;
    for (int i = 0; i < 32; ++i) {
        const AdaptiveQualitySample& decision = fixture.stage.observe(500.0, std::nullopt);
        if (!expect(!decision.changed, "a disabled ladder never changes")) {
            return false;
        }
    }
    return expect(fixture.fake.reduce_effect_calls == 0,
                  "a disabled ladder applies no effect flip") &&
           expect(fixture.fake.animation_pause_calls == 0,
                  "a disabled ladder pauses no animations");
}

bool check_publish_records_the_decision_without_observing() {
    Fixture fixture{enabled_config()};
    fixture.stage.publish();
    return expect(fixture.stage.decision().render_scale == 1.0f,
                  "the initial decision is full quality") &&
           expect(fixture.fake.reduce_effect_calls == 0, "publishing applies nothing") &&
           expect(fixture.stage.decision() == fixture.diagnostics.adaptive_quality(),
                  "publish records the decision in force in diagnostics");
}

bool check_a_non_finite_frame_time_is_not_an_observation() {
    Fixture fixture{enabled_config()};
    const AdaptiveQualitySample before = fixture.stage.decision();
    static_cast<void>(fixture.stage.observe(0.0, std::nullopt));
    static_cast<void>(fixture.stage.observe(-1.0, std::nullopt));
    return expect(fixture.stage.decision() == before,
                  "a zero or negative frame time changes nothing") &&
           expect(fixture.fake.reduce_effect_calls == 0, "and applies nothing");
}

} // namespace

int main() {
    bool ok = true;
    ok = check_on_time_frames_do_not_change_anything() && ok;
    ok = check_gpu_interval_wins() && ok;
    ok = check_missing_gpu_interval_is_a_fallback() && ok;
    ok = check_slow_frames_reduce_effects_and_request_one_repaint() && ok;
    ok = check_paused_animations_wake_the_loop() && ok;
    ok = check_disabled_ladder_never_moves() && ok;
    ok = check_publish_records_the_decision_without_observing() && ok;
    ok = check_a_non_finite_frame_time_is_not_an_observation() && ok;
    if (!ok) {
        std::cerr << "app_quality_stage: FAILED\n";
        return 1;
    }
    std::cout << "app_quality_stage: all checks passed\n";
    return 0;
}
