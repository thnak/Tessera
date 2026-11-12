// SPDX-License-Identifier: MIT
//
// The deterministic adaptive-quality decision layer (docs/design/performance.md section 6,
// docs/specs/runtime-config-v1.md sections 5.6, 5.7 and 6.2).
//
// One frame observation in, one decision out: no clock, no RNG, no locale, no allocation (not even
// at construction). This slice decides and reports; it does NOT scale a render target. The
// configured `renderScale` has no consumer yet, so a downgrade here changes a reported number,
// never a pixel. The follow-up slice is the render-target consumer.

#pragma once

#include "tessera/runtime/config.hpp"
#include "tessera/runtime/diagnostics.hpp"

#include <cstdint>

namespace tessera::runtime {

// Bounded policy constants. These exact numbers are the contract and are recorded in
// docs/design/performance.md section 6. The reasoning:
//   - 4 consecutive misses: one slow frame is noise (a GC pause, a driver hiccup, a resize); four
//     in a row at 60 Hz is ~66 ms of sustained overrun, which is a real load change.
//   - 12 consecutive met frames: recovery is deliberately slower than the downgrade (hysteresis);
//     ~200 ms of headroom before quality goes back up, so a machine that is merely borderline does
//     not ping-pong.
//   - 8 observed frames of cooldown: no two changes within it, so a change is at most one step per
//     ~133 ms at 60 Hz and the renderer never sees per-frame resolution churn.
inline constexpr std::uint32_t kAdaptiveQualityDowngradeMisses = 4;
inline constexpr std::uint32_t kAdaptiveQualityUpgradeHits = 12;
inline constexpr std::uint32_t kAdaptiveQualityCooldownFrames = 8;
// Ladder size: step 0 is the configured `renderScale`, step N is `minRenderScale`, intermediate
// steps are evenly spaced. A bounded ladder is what makes "every change is one visible step" true;
// there is no per-frame fractional creep.
inline constexpr std::uint32_t kAdaptiveQualitySteps = 4;
// The `renderScale` / `minRenderScale` range from the runtime-config field table.
inline constexpr float kAdaptiveQualityMinScale = 0.25f;
inline constexpr float kAdaptiveQualityMaxScale = 4.0f;

struct AdaptiveQualityConfig {
    // `adaptiveQuality`. False disables the controller completely.
    bool adaptive_quality = true;
    // `powerPolicy`. `Benchmark` force-disables the controller: adaptive quality during profiling
    // would corrupt the measurement.
    PowerPolicy power_policy = PowerPolicy::Adaptive;
    // `renderScale`: the full-quality scale (step 0) and the upper clamp.
    float render_scale = 1.0f;
    // `minRenderScale`: the lower clamp and the last ladder step.
    float min_render_scale = 0.6f;
    // `frameBudgetMs`; 0 derives the budget from `target_fps`.
    double frame_budget_ms = 0.0;
    // The effective present rate used to derive the budget when `frame_budget_ms` is 0. 0 means no
    // budget, so there is no evidence and the controller never changes anything.
    double target_fps = 60.0;
};

// A small hysteresis state machine fed one frame observation at a time.
//
// Determinism: every member is a scalar (trivially copyable, trivially destructible, no owned
// storage), observe() is a pure function of the current state plus the argument, and no clock, RNG
// or locale is consulted. The same observation sequence on the same config always yields the same
// decision sequence.
class AdaptiveQualityController {
public:
    // The documented defaults: adaptive on, 60 Hz budget derived from the default target rate.
    AdaptiveQualityController();
    explicit AdaptiveQualityController(const AdaptiveQualityConfig& config);

    // Feeds one observation: `frame_time_ms` is the measured time the frame took (CPU+GPU where a
    // GPU number exists), in milliseconds. Returns the decision in force after the observation;
    // `changed`/`downgrade` are true only on the frame that produced a change. A non-finite or
    // non-positive time is not an observation: it neither counts toward a run nor changes anything.
    [[nodiscard]] AdaptiveQualitySample observe(double frame_time_ms) noexcept;

    // Back to full quality with no run/cooldown state. Deterministic: it does not consult the
    // clock.
    void reset() noexcept;

    [[nodiscard]] const AdaptiveQualitySample& decision() const noexcept {
        return decision_;
    }
    [[nodiscard]] const AdaptiveQualityConfig& config() const noexcept {
        return config_;
    }
    // True when both the config switch and the policy allow changes and a usable budget exists.
    [[nodiscard]] bool active() const noexcept {
        return active_;
    }
    // The budget the observations are compared against: `frameBudgetMs`, else 1000 / `target_fps`,
    // else 0 (no budget).
    [[nodiscard]] double budget_ms() const noexcept {
        return budget_ms_;
    }
    [[nodiscard]] std::uint32_t misses() const noexcept {
        return misses_;
    }
    [[nodiscard]] std::uint32_t hits() const noexcept {
        return hits_;
    }
    [[nodiscard]] std::uint32_t cooldown_remaining() const noexcept {
        return cooldown_;
    }

private:
    // Step 0 is exactly `render_scale`, the last step exactly `min_render_scale`; the steps between
    // are evenly spaced. Exact at both ends so the clamps land on the configured values.
    [[nodiscard]] float scale_for_step(std::uint32_t step) const noexcept;

    AdaptiveQualityConfig config_{};
    AdaptiveQualitySample decision_{};
    double budget_ms_ = 0.0;
    std::uint32_t step_ = 0;
    std::uint32_t misses_ = 0;
    std::uint32_t hits_ = 0;
    std::uint32_t cooldown_ = 0;
    bool active_ = false;
};

} // namespace tessera::runtime
