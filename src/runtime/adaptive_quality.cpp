// SPDX-License-Identifier: MIT

#include "adaptive_quality.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::runtime {
namespace {

float checked_scale(float value, float fallback, float low, float high) {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::clamp(value, low, high);
}

} // namespace

AdaptiveQualityController::AdaptiveQualityController()
    : AdaptiveQualityController(AdaptiveQualityConfig{}) {}

AdaptiveQualityController::AdaptiveQualityController(const AdaptiveQualityConfig& config)
    : config_(config) {
    // Normalize once so observe() has no error branches. The loader already rejects
    // `minRenderScale > renderScale` (CrossField), but the controller is also constructible
    // directly, so it clamps a bad pair instead of trusting the caller.
    config_.render_scale = checked_scale(config_.render_scale, 1.0f, kAdaptiveQualityMinScale,
                                         kAdaptiveQualityMaxScale);
    config_.min_render_scale = checked_scale(config_.min_render_scale, config_.render_scale,
                                             kAdaptiveQualityMinScale, config_.render_scale);
    if (!std::isfinite(config_.frame_budget_ms) || config_.frame_budget_ms < 0.0) {
        config_.frame_budget_ms = 0.0;
    }
    if (!std::isfinite(config_.target_fps) || config_.target_fps <= 0.0) {
        config_.target_fps = 0.0;
    }
    budget_ms_ = config_.frame_budget_ms > 0.0
                     ? config_.frame_budget_ms
                     : (config_.target_fps > 0.0 ? 1000.0 / config_.target_fps : 0.0);
    // An equal or inverted min/render pair has no room to move, so it is not a disabled switch but
    // an empty ladder: no observation can produce a change (runtime-config-v1 rule 13 leaves both
    // equal values valid).
    active_ = config_.adaptive_quality && config_.power_policy != PowerPolicy::Benchmark &&
              config_.min_render_scale < config_.render_scale && budget_ms_ > 0.0;
    decision_ = AdaptiveQualitySample{config_.render_scale, 0, false, false};
}

AdaptiveQualitySample AdaptiveQualityController::observe(double frame_time_ms) noexcept {
    // `changed`/`downgrade` describe this observation only; the scale and step persist.
    decision_.changed = false;
    decision_.downgrade = false;
    if (!active_ || !std::isfinite(frame_time_ms) || frame_time_ms <= 0.0) {
        return decision_;
    }
    if (cooldown_ > 0) {
        // A change just happened. Frames inside the cooldown are not evidence for the next run:
        // counting them would shorten the next threshold by the cooldown length and let a sustained
        // overload step down once per cooldown frame.
        --cooldown_;
        return decision_;
    }
    if (frame_time_ms > budget_ms_) {
        hits_ = 0;
        // Capped so the counter cannot wrap and so an overload at the floor is a no-op.
        misses_ = std::min(misses_ + 1, kAdaptiveQualityDowngradeMisses);
        if (misses_ >= kAdaptiveQualityDowngradeMisses && step_ < kAdaptiveQualitySteps) {
            ++step_;
            misses_ = 0;
            cooldown_ = kAdaptiveQualityCooldownFrames;
            decision_.render_scale = scale_for_step(step_);
            decision_.quality_step = step_;
            decision_.changed = true;
            decision_.downgrade = true;
        }
        return decision_;
    }
    misses_ = 0;
    hits_ = std::min(hits_ + 1, kAdaptiveQualityUpgradeHits);
    if (hits_ >= kAdaptiveQualityUpgradeHits && step_ > 0) {
        --step_;
        hits_ = 0;
        cooldown_ = kAdaptiveQualityCooldownFrames;
        decision_.render_scale = scale_for_step(step_);
        decision_.quality_step = step_;
        decision_.changed = true;
    }
    return decision_;
}

void AdaptiveQualityController::reset() noexcept {
    step_ = 0;
    misses_ = 0;
    hits_ = 0;
    cooldown_ = 0;
    decision_ = AdaptiveQualitySample{config_.render_scale, 0, false, false};
}

float AdaptiveQualityController::scale_for_step(std::uint32_t step) const noexcept {
    // Exact endpoints: the clamp promises the configured values, not an interpolation of them.
    if (step == 0) {
        return config_.render_scale;
    }
    if (step >= kAdaptiveQualitySteps) {
        return config_.min_render_scale;
    }
    const double t = static_cast<double>(step) / static_cast<double>(kAdaptiveQualitySteps);
    const double scale =
        static_cast<double>(config_.render_scale) + (static_cast<double>(config_.min_render_scale) -
                                                     static_cast<double>(config_.render_scale)) *
                                                        t;
    return static_cast<float>(scale);
}

} // namespace tessera::runtime
