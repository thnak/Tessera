// SPDX-License-Identifier: MIT
//
// The adaptive present cap (docs/design/performance.md section 6, step 3;
// docs/specs/runtime-config-v1.md sections 5.2 and 5.7).
//
// The divisor rule is a pure function of (display refresh, configured cap, quality step), so these
// cases pin it without a window, a pacer or a clock: exact divisors, the documented floor, the
// `maxFps` ceiling, an unknown refresh (no divisor) and step 0 as identity. The last case proves
// the rule reaches the pacer through its existing scripted-rate-change path: the effective present
// interval changes at the documented boundary and returns on upgrade.

#include "adaptive_quality.hpp"
#include "present_cap.hpp"
#include "tessera/runtime/frame_pacer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>

namespace {

using tessera::runtime::adaptive_present_cap_hz;
using tessera::runtime::AdaptiveQualityConfig;
using tessera::runtime::AdaptiveQualityController;
using tessera::runtime::AdaptiveQualitySample;
using tessera::runtime::FramePacer;
using tessera::runtime::FramePacerConfig;
using tessera::runtime::kAdaptivePresentCapFloorHz;
using tessera::runtime::kAdaptiveQualityCooldownFrames;
using tessera::runtime::kAdaptiveQualityDowngradeMisses;
using tessera::runtime::kAdaptiveQualitySteps;
using tessera::runtime::kAdaptiveQualityUpgradeHits;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "present_cap: " << message << "\n";
    }
    return condition;
}

bool near(double value, double expected, double tolerance = 1.0e-9) {
    return std::abs(value - expected) <= tolerance;
}

// --- 1. Step 0 is identity ---------------------------------------------------------------------

bool step_zero_is_identity() {
    bool ok = true;
    // Whatever the configured cap and refresh, step 0 never moves the cap: full quality presents at
    // exactly the configured rate.
    for (const double refresh : {0.0, 60.0, 144.0, 165.0}) {
        for (const double cap : {0.0, 30.0, 60.0, 72.0, 120.0}) {
            ok = expect(adaptive_present_cap_hz(refresh, cap, 0) == cap,
                        "step 0 returns the configured cap unchanged") &&
                 ok;
        }
    }
    return ok;
}

// --- 2. Exact divisors -------------------------------------------------------------------------

bool exact_divisors_walk_down() {
    bool ok = true;
    // 60 Hz, uncapped: step 1 is refresh/2, step 2 refresh/3, step 3 refresh/4, step 4 refresh/5.
    ok = expect(near(adaptive_present_cap_hz(60.0, 0.0, 1), 30.0), "60 Hz step 1 is refresh/2") &&
         ok;
    ok = expect(near(adaptive_present_cap_hz(60.0, 0.0, 2), 20.0), "60 Hz step 2 is refresh/3") &&
         ok;
    ok = expect(near(adaptive_present_cap_hz(60.0, 0.0, 3), 15.0), "60 Hz step 3 is refresh/4") &&
         ok;
    ok = expect(near(adaptive_present_cap_hz(60.0, 0.0, 4), 12.0), "60 Hz step 4 is refresh/5") &&
         ok;

    // 144 Hz, uncapped: 72, 48, 36, 28.8.
    ok = expect(near(adaptive_present_cap_hz(144.0, 0.0, 1), 72.0), "144 Hz step 1 is refresh/2") &&
         ok;
    ok = expect(near(adaptive_present_cap_hz(144.0, 0.0, 2), 48.0), "144 Hz step 2 is refresh/3") &&
         ok;
    ok = expect(near(adaptive_present_cap_hz(144.0, 0.0, 3), 36.0), "144 Hz step 3 is refresh/4") &&
         ok;
    ok = expect(near(adaptive_present_cap_hz(144.0, 0.0, 4), 28.8), "144 Hz step 4 is refresh/5") &&
         ok;

    // A cap that is already a divisor moves to the *next* divisor, never to itself: 60 Hz capped at
    // 30 (refresh/2) goes to refresh/3, not back to 30.
    ok = expect(near(adaptive_present_cap_hz(60.0, 30.0, 1), 20.0),
                "a cap on a divisor steps to the next divisor") &&
         ok;
    // 144 Hz capped at 72 (refresh/2) -> refresh/3.
    ok = expect(near(adaptive_present_cap_hz(144.0, 72.0, 1), 48.0), "144/72 steps to refresh/3") &&
         ok;
    // A cap that is not a divisor: 144 Hz capped at 50 -> floor(144/50)+1 = 3 -> 48.
    ok = expect(near(adaptive_present_cap_hz(144.0, 50.0, 1), 48.0),
                "a non-divisor cap steps to the next divisor below it") &&
         ok;
    return ok;
}

// --- 3. The floor ------------------------------------------------------------------------------

bool the_floor_holds() {
    bool ok = true;
    // 60 Hz: 30, 20, 15, 12, then refresh/6 = 10 (the floor), and every further step stays there.
    ok = expect(near(adaptive_present_cap_hz(60.0, 0.0, 5), 10.0),
                "60 Hz step 5 reaches the floor") &&
         ok;
    for (std::uint32_t step = 5; step < 40; ++step) {
        ok = expect(near(adaptive_present_cap_hz(60.0, 0.0, step), kAdaptivePresentCapFloorHz),
                    "the cap never goes below the documented floor") &&
             ok;
    }
    // A low refresh rate would divide below the floor: 24 Hz step 1 is refresh/2 = 12, and step 2
    // would be refresh/3 = 8, so the cap holds at 12 instead of dropping under the floor.
    ok = expect(near(adaptive_present_cap_hz(24.0, 0.0, 1), 12.0), "24 Hz step 1 is refresh/2") &&
         ok;
    ok = expect(near(adaptive_present_cap_hz(24.0, 0.0, 2), 12.0),
                "24 Hz step 2 holds at the last divisor above the floor") &&
         ok;
    ok = expect(near(adaptive_present_cap_hz(24.0, 0.0, 9), 12.0), "the floor is a fixed point") &&
         ok;
    // Every value the rule returns after a downgrade is a real refresh divisor: the floor stops the
    // walk instead of clamping to a rate the display cannot hit. (Step 0 is the configured cap,
    // which may be 0 = uncapped, so it is not part of this check.)
    for (const double refresh : {24.0, 60.0, 144.0}) {
        for (std::uint32_t step = 1; step <= 12; ++step) {
            const double cap = adaptive_present_cap_hz(refresh, 0.0, step);
            const double divisor = refresh / cap;
            ok = expect(cap >= kAdaptivePresentCapFloorHz,
                        "no divisor-derived cap goes below the floor") &&
                 ok;
            ok = expect(near(divisor, std::round(divisor), 1.0e-9),
                        "every cap is a whole refresh divisor") &&
                 ok;
        }
    }
    return ok;
}

// --- 4. The maxFps ceiling ---------------------------------------------------------------------

bool the_configured_cap_is_a_ceiling() {
    bool ok = true;
    // 144 Hz capped at 60: step 1 is the next divisor below 60, which is refresh/3 = 48 (not 72,
    // which would *raise* the rate above the configured cap).
    ok = expect(near(adaptive_present_cap_hz(144.0, 60.0, 1), 48.0),
                "the adaptive cap never rises above maxFps") &&
         ok;
    // 60 Hz capped at 30: step 1 is 20, below the cap.
    ok = expect(near(adaptive_present_cap_hz(60.0, 30.0, 1), 20.0),
                "a capped 60 Hz display steps below the cap") &&
         ok;
    // A cap above the refresh is clamped by the refresh: 60 Hz capped at 120 presents at 60, so
    // step 1 is refresh/2 = 30, not 60/2 of the cap.
    ok = expect(near(adaptive_present_cap_hz(60.0, 120.0, 1), 30.0),
                "a cap above the refresh is clamped by the refresh") &&
         ok;
    // Every step is monotonically non-increasing and never above the configured cap. A cap of 0
    // means uncapped, so it is compared as the refresh rate it lets through, not as the number 0.
    const auto effective = [](double refresh, double cap) {
        if (cap > 0.0) {
            return refresh > 0.0 ? std::min(cap, refresh) : cap;
        }
        return refresh;
    };
    for (const double refresh : {60.0, 144.0, 165.0}) {
        for (const double cap : {0.0, 30.0, 60.0, 72.0, 120.0}) {
            double previous = effective(refresh, adaptive_present_cap_hz(refresh, cap, 0));
            for (std::uint32_t step = 1; step <= 8; ++step) {
                const double current =
                    effective(refresh, adaptive_present_cap_hz(refresh, cap, step));
                ok = expect(current <= previous + 1.0e-9,
                            "each step lowers or holds the effective rate") &&
                     ok;
                ok = expect(cap == 0.0 || current <= cap + 1.0e-9,
                            "no step exceeds the configured maxFps") &&
                     ok;
                ok = expect(current <= refresh + 1.0e-9, "no step exceeds the refresh rate") && ok;
                previous = current;
            }
        }
    }
    return ok;
}

// --- 5. Unknown refresh: no divisor ------------------------------------------------------------

bool unknown_refresh_has_no_divisor() {
    bool ok = true;
    // No refresh: the cap is the configured one at every step, so the pacer keeps its documented
    // deadline pacing (runtime-config-v1.md section 5.2) instead of inventing a divisor.
    for (std::uint32_t step = 0; step <= 8; ++step) {
        ok = expect(adaptive_present_cap_hz(0.0, 60.0, step) == 60.0,
                    "an unknown refresh leaves the configured cap in place") &&
             ok;
        ok = expect(adaptive_present_cap_hz(0.0, 0.0, step) == 0.0,
                    "an unknown refresh and no cap stays uncapped") &&
             ok;
    }
    // A non-finite or negative refresh is unknown, not a rate.
    ok = expect(adaptive_present_cap_hz(-60.0, 30.0, 3) == 30.0,
                "a negative refresh is treated as unknown") &&
         ok;
    ok = expect(adaptive_present_cap_hz(std::nan(""), 30.0, 3) == 30.0,
                "a NaN refresh is treated as unknown") &&
         ok;
    // A bad configured cap is treated as uncapped, never as a negative rate the pacer would reject.
    ok = expect(adaptive_present_cap_hz(60.0, -30.0, 0) == 0.0,
                "a negative configured cap is treated as uncapped") &&
         ok;
    ok = expect(adaptive_present_cap_hz(60.0, std::nan(""), 1) == 30.0,
                "a NaN configured cap is treated as uncapped") &&
         ok;
    return ok;
}

// --- 6. The cap reaches the pacer through the scripted rate-change path ------------------------

bool the_cap_reaches_the_pacer() {
    bool ok = true;
    // The pacer starts at the configured cap on a 60 Hz display: 60/60 = 1 refresh per frame.
    FramePacer pacer(FramePacerConfig{60.0, 60.0, false});
    ok = expect(near(pacer.interval_seconds(), 1.0 / 60.0),
                "the pacer starts at the configured cap");
    pacer.present(0.0);

    // A downgrade to step 1: the cap is refresh/2 = 30, so the interval doubles. The change is
    // applied through update() - the existing scripted-rate-change mechanism - at a frame boundary.
    const double change = 1.0 / 60.0;
    const double step_one_cap = adaptive_present_cap_hz(60.0, 60.0, 1);
    ok = expect(near(step_one_cap, 30.0), "step 1 of a 60 Hz capped display is 30 Hz") && ok;
    pacer.update(FramePacerConfig{step_one_cap, 60.0, false}, change);
    ok = expect(near(pacer.interval_seconds(), 2.0 / 60.0),
                "the downgraded cap doubles the present interval") &&
         ok;
    // No burst: the change instant is not a deadline, the next frame is one new interval later.
    auto decision = pacer.ready(change);
    ok = expect(!decision.render_now, "the cap change does not burst a frame") && ok;
    ok = expect(near(decision.wait_seconds, 2.0 / 60.0),
                "the cap change waits exactly one new interval") &&
         ok;
    decision = pacer.ready(change + 2.0 / 60.0);
    ok = expect(decision.render_now, "the frame renders at the new deadline") && ok;
    pacer.present(change + 2.0 / 60.0);

    // A second downgrade: refresh/3 = 20 Hz, three refreshes per present.
    const double second_change = change + 2.0 / 60.0;
    const double step_two_cap = adaptive_present_cap_hz(60.0, 60.0, 2);
    ok = expect(near(step_two_cap, 20.0), "step 2 of a 60 Hz capped display is 20 Hz") && ok;
    pacer.update(FramePacerConfig{step_two_cap, 60.0, false}, second_change);
    ok = expect(near(pacer.interval_seconds(), 3.0 / 60.0),
                "the second downgrade triples the present interval") &&
         ok;

    // An upgrade restores the previous divisor: back to step 1's 30 Hz, then to the configured cap.
    const double upgrade = second_change + 3.0 / 60.0;
    pacer.update(FramePacerConfig{step_one_cap, 60.0, false}, upgrade);
    ok = expect(near(pacer.interval_seconds(), 2.0 / 60.0),
                "an upgrade restores the previous divisor") &&
         ok;
    const double restore = upgrade + 2.0 / 60.0;
    pacer.update(FramePacerConfig{60.0, 60.0, false}, restore);
    ok = expect(near(pacer.interval_seconds(), 1.0 / 60.0),
                "the final upgrade restores the configured cap") &&
         ok;
    decision = pacer.ready(restore);
    ok = expect(!decision.render_now, "the restore waits one interval") && ok;
    decision = pacer.ready(restore + 1.0 / 60.0);
    ok = expect(decision.render_now, "the restored cadence renders at the next refresh") && ok;

    // The unknown-refresh fallback: the cap is the configured one, so the pacer keeps deadline
    // pacing at 1/max_fps rather than a divisor.
    FramePacer unknown(FramePacerConfig{60.0, 0.0, false});
    ok = expect(near(unknown.interval_seconds(), 1.0 / 60.0),
                "an unknown refresh paces on the cap's deadline") &&
         ok;
    const double unknown_cap = adaptive_present_cap_hz(0.0, 60.0, 3);
    unknown.update(FramePacerConfig{unknown_cap, 0.0, false}, 1.0 / 60.0);
    ok = expect(near(unknown.interval_seconds(), 1.0 / 60.0),
                "an unknown refresh never invents a divisor") &&
         ok;
    return ok;
}

// One over-budget observation and one inside the budget, far enough from the 16 ms boundary that
// no floating-point noise can decide the comparison.
constexpr double kMissMs = 20.0;
constexpr double kMetMs = 10.0;

// render_scale=1.0 and min_render_scale=0.5 make every ladder step exactly representable, so the
// tests compare floats exactly instead of with an epsilon.
AdaptiveQualityConfig base_config() {
    AdaptiveQualityConfig config;
    config.adaptive_quality = true;
    config.power_policy = tessera::PowerPolicy::Adaptive;
    config.render_scale = 1.0f;
    config.min_render_scale = 0.5f;
    config.frame_budget_ms = 16.0;
    config.target_fps = 60.0;
    return config;
}

// A 60 Hz display with no configured cap: the step-3 divisor base is the refresh itself, so the
// documented cap sequence is 0 (step 0, uncapped), 30, 20, 15, 12.
AdaptiveQualityConfig capped_config() {
    AdaptiveQualityConfig config = base_config();
    config.max_fps = 0.0;
    config.refresh_hz = 60.0;
    return config;
}

// --- Controller: the present cap is a pure function of the ladder step ------------------------

// --- 6. The present cap (step 3) --------------------------------------------------------------

// Drives one full downgrade run (cooldown + misses) and returns the decision after it.
AdaptiveQualitySample downgrade_once(AdaptiveQualityController& controller) {
    for (std::uint32_t frame = 0; frame < kAdaptiveQualityCooldownFrames; ++frame) {
        (void)controller.observe(kMetMs);
    }
    AdaptiveQualitySample sample{};
    for (std::uint32_t frame = 0; frame < kAdaptiveQualityDowngradeMisses; ++frame) {
        sample = controller.observe(kMissMs);
    }
    return sample;
}

bool downgrade_run_produces_the_documented_cap_sequence() {
    AdaptiveQualityController controller(capped_config());
    // Step 0 is the configured cap, which is 0 here: uncapped, so the pacer follows the 60 Hz
    // refresh. The documented sequence after that is 30, 20, 15, 12.
    bool ok = true;
    ok = expect(controller.decision().present_cap_hz == 0.0,
                "step 0 keeps the configured cap (uncapped)") &&
         ok;
    const double expected[kAdaptiveQualitySteps] = {30.0, 20.0, 15.0, 12.0};
    for (std::uint32_t step = 1; step <= kAdaptiveQualitySteps; ++step) {
        const AdaptiveQualitySample sample = downgrade_once(controller);
        ok = expect(sample.changed && sample.downgrade, "each run downgrades one step") && ok;
        ok = expect(sample.quality_step == step, "the ladder index advances by one") && ok;
        ok = expect(sample.present_cap_hz == expected[step - 1],
                    "the cap is the next refresh divisor below the previous rate") &&
             ok;
    }
    // A sustained overload at the last step changes nothing more: the cap is at the floor of the
    // ladder, not below it.
    const AdaptiveQualitySample floor_sample = controller.observe(kMissMs);
    ok = expect(!floor_sample.changed, "the last ladder step is a fixed point") && ok;
    ok = expect(floor_sample.present_cap_hz == 12.0, "the cap holds at the last step") && ok;
    return ok;
}

bool upgrade_restores_the_previous_divisor() {
    AdaptiveQualityController controller(capped_config());
    // Walk to step 3 (15 Hz) and remember the caps on the way down.
    double caps[kAdaptiveQualitySteps + 1] = {};
    caps[0] = controller.decision().present_cap_hz;
    for (std::uint32_t step = 1; step <= 3; ++step) {
        caps[step] = downgrade_once(controller).present_cap_hz;
    }
    bool ok = true;
    ok = expect(caps[3] == 15.0, "the walk down reached refresh/4") && ok;
    // One upgrade run returns to the previous divisor, exactly the cap the same step had on the way
    // down: the rule is a pure function of the step, so no separate "previous divisor" state can
    // drift.
    for (std::uint32_t frame = 0; frame < kAdaptiveQualityCooldownFrames; ++frame) {
        (void)controller.observe(kMetMs);
    }
    AdaptiveQualitySample sample{};
    for (std::uint32_t frame = 0; frame < kAdaptiveQualityUpgradeHits; ++frame) {
        sample = controller.observe(kMetMs);
    }
    ok = expect(sample.changed && !sample.downgrade, "the met run upgrades one step") && ok;
    ok = expect(sample.quality_step == 2, "the upgrade moves one ladder step up") && ok;
    ok =
        expect(sample.present_cap_hz == caps[2], "the upgrade restores the previous divisor") && ok;
    ok = expect(sample.present_cap_hz == 20.0, "step 2's cap is refresh/3") && ok;

    // All the way back to step 0: the configured cap again.
    for (std::uint32_t step = 2; step > 0; --step) {
        for (std::uint32_t frame = 0; frame < kAdaptiveQualityCooldownFrames; ++frame) {
            (void)controller.observe(kMetMs);
        }
        for (std::uint32_t frame = 0; frame < kAdaptiveQualityUpgradeHits; ++frame) {
            sample = controller.observe(kMetMs);
        }
    }
    ok = expect(sample.quality_step == 0, "the recovery returns to step 0") && ok;
    ok = expect(sample.present_cap_hz == caps[0], "step 0 restores the configured cap") && ok;
    return ok;
}

bool the_configured_cap_is_a_hard_ceiling() {
    // 144 Hz capped at 60: the effective rate is 60, so step 1 is refresh/3 = 48, never 72.
    AdaptiveQualityConfig config = capped_config();
    config.refresh_hz = 144.0;
    config.max_fps = 60.0;
    AdaptiveQualityController controller(config);
    bool ok = true;
    ok = expect(controller.decision().present_cap_hz == 60.0, "step 0 is the configured maxFps") &&
         ok;
    const AdaptiveQualitySample first = downgrade_once(controller);
    ok = expect(first.present_cap_hz == 48.0,
                "the adaptive cap only lowers the rate below maxFps") &&
         ok;
    ok = expect(first.present_cap_hz < config.max_fps, "the cap never rises above maxFps") && ok;
    ok =
        expect(first.present_cap_hz < config.refresh_hz, "the cap never rises above the refresh") &&
        ok;
    return ok;
}

bool unknown_refresh_keeps_the_configured_cap() {
    // No display refresh: there is no divisor, so the cap stays at maxFps and the pacer keeps its
    // documented deadline pacing. The scale still moves - only the cap is inert.
    AdaptiveQualityConfig config = capped_config();
    config.refresh_hz = 0.0;
    config.max_fps = 60.0;
    AdaptiveQualityController controller(config);
    bool ok = true;
    for (std::uint32_t step = 1; step <= kAdaptiveQualitySteps; ++step) {
        const AdaptiveQualitySample sample = downgrade_once(controller);
        ok = expect(sample.changed, "the scale still downgrades without a refresh") && ok;
        ok = expect(sample.present_cap_hz == 60.0,
                    "an unknown refresh leaves the configured cap in place") &&
             ok;
    }
    return ok;
}

bool disabled_paths_never_change_the_cap() {
    bool ok = true;
    AdaptiveQualityConfig off = capped_config();
    off.max_fps = 60.0;
    off.adaptive_quality = false;
    AdaptiveQualityController off_controller(off);
    AdaptiveQualityConfig benchmark = capped_config();
    benchmark.max_fps = 60.0;
    benchmark.power_policy = tessera::PowerPolicy::Benchmark;
    AdaptiveQualityController benchmark_controller(benchmark);
    for (int frame = 0; frame < 200; ++frame) {
        const AdaptiveQualitySample off_sample = off_controller.observe(kMissMs);
        const AdaptiveQualitySample benchmark_sample = benchmark_controller.observe(kMissMs);
        ok = expect(off_sample.present_cap_hz == off.max_fps,
                    "adaptiveQuality=false never changes the cap") &&
             ok;
        ok = expect(benchmark_sample.present_cap_hz == benchmark.max_fps,
                    "powerPolicy=benchmark never changes the cap") &&
             ok;
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = step_zero_is_identity() && ok;
    ok = exact_divisors_walk_down() && ok;
    ok = the_floor_holds() && ok;
    ok = the_configured_cap_is_a_ceiling() && ok;
    ok = unknown_refresh_has_no_divisor() && ok;
    ok = the_cap_reaches_the_pacer() && ok;
    ok = downgrade_run_produces_the_documented_cap_sequence() && ok;
    ok = upgrade_restores_the_previous_divisor() && ok;
    ok = the_configured_cap_is_a_hard_ceiling() && ok;
    ok = unknown_refresh_keeps_the_configured_cap() && ok;
    ok = disabled_paths_never_change_the_cap() && ok;
    if (!ok) {
        std::cerr << "present_cap: FAILED\n";
        return 1;
    }
    std::cout << "present_cap: ok\n";
    return 0;
}
