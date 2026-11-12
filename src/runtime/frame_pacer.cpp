// SPDX-License-Identifier: MIT

#include "tessera/runtime/frame_pacer.hpp"

#include <cmath>
#include <stdexcept>

namespace tessera::runtime {
namespace {

constexpr double kEpsilon = 1.0e-12;
// Relative slack when matching `max_fps` against a refresh divisor: display rates arrive as
// doubles (59.94, 143.97) and should still count as an integer ratio.
constexpr double kDivisorTolerance = 1.0e-6;

bool is_valid_non_negative(double value) {
    return std::isfinite(value) && value >= 0.0;
}

void validate_config(const FramePacerConfig& config) {
    if (!is_valid_non_negative(config.max_fps)) {
        throw std::invalid_argument("max_fps must be finite and non-negative");
    }
    if (!is_valid_non_negative(config.refresh_hz)) {
        throw std::invalid_argument("refresh_hz must be finite and non-negative");
    }
}

// The present cadence of a policy: 0 means "present when ready".
double derive_interval(const FramePacerConfig& config) {
    if (config.vrr_active) {
        // The display follows the content rate, so only an explicit cap schedules a wait.
        return config.max_fps > 0.0 ? 1.0 / config.max_fps : 0.0;
    }
    if (config.max_fps <= 0.0) {
        return config.refresh_hz > 0.0 ? 1.0 / config.refresh_hz : 0.0;
    }
    if (config.refresh_hz > 0.0) {
        // Prefer the refresh divisor: `max_fps` is (within tolerance) `refresh / k`, so present
        // every k-th refresh. This keeps the deadline on the display grid; 144 Hz + 72 -> 2/144.
        const double ratio = config.refresh_hz / config.max_fps;
        const double divisor = std::round(ratio);
        if (divisor >= 1.0 && std::abs(ratio - divisor) <= kDivisorTolerance * divisor) {
            return divisor / config.refresh_hz;
        }
    }
    return 1.0 / config.max_fps;
}

} // namespace

FramePacer::FramePacer(FramePacerConfig config) {
    update(config, 0.0);
}

void FramePacer::update(FramePacerConfig config, double now_seconds) {
    validate_config(config);
    if (!is_valid_non_negative(now_seconds)) {
        throw std::invalid_argument("pacer time must be finite and non-negative");
    }

    const double interval = derive_interval(config);
    const bool cadence_changed = std::abs(interval - interval_seconds_) > kEpsilon;
    max_fps_ = config.max_fps;
    refresh_hz_ = config.refresh_hz;
    vrr_active_ = config.vrr_active;
    interval_seconds_ = interval;

    // A policy change takes effect from the instant it arrives. Keeping the old deadline would
    // replay slots the new slower rate does not have, or hold the first frame at the old slower
    // cadence after a speed-up. Resyncing to `now + interval` neither bursts nor stalls.
    if (started_ && cadence_changed) {
        next_deadline_ = now_seconds + interval_seconds_;
    }
}

PaceDecision FramePacer::ready(double now_seconds) {
    if (!is_valid_non_negative(now_seconds)) {
        throw std::invalid_argument("pacer time must be finite and non-negative");
    }
    if (interval_seconds_ <= 0.0 || !started_) {
        return PaceDecision{true, 0.0, false};
    }

    const bool late = now_seconds > next_deadline_ + kEpsilon;
    if (now_seconds + kEpsilon >= next_deadline_) {
        return PaceDecision{true, 0.0, late};
    }

    return PaceDecision{false, next_deadline_ - now_seconds, false};
}

void FramePacer::present(double now_seconds) {
    if (!is_valid_non_negative(now_seconds)) {
        throw std::invalid_argument("pacer time must be finite and non-negative");
    }
    if (interval_seconds_ <= 0.0) {
        started_ = true;
        next_deadline_ = now_seconds;
        return;
    }

    if (!started_) {
        started_ = true;
        next_deadline_ = now_seconds + interval_seconds_;
        return;
    }

    // Keep the cadence when this frame only ran a little past its deadline: the next deadline is
    // one interval after the old one, so render time does not stretch every period. Resync to
    // `now` only once a whole interval was missed, otherwise the pacer would burst to catch up.
    const double on_schedule = next_deadline_ + interval_seconds_;
    next_deadline_ =
        now_seconds + kEpsilon >= on_schedule ? now_seconds + interval_seconds_ : on_schedule;
}

void FramePacer::reset() {
    next_deadline_ = 0.0;
    started_ = false;
}

} // namespace tessera::runtime
