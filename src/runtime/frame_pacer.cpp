// SPDX-License-Identifier: MIT

#include "tessera/runtime/frame_pacer.hpp"

#include <cmath>
#include <stdexcept>

namespace tessera::runtime {
namespace {

constexpr double kEpsilon = 1.0e-12;

bool is_valid_seconds(double value) {
    return std::isfinite(value) && value >= 0.0;
}

} // namespace

FramePacer::FramePacer(FramePacerConfig config)
    : max_fps_(config.max_fps) {
    if (!std::isfinite(max_fps_) || max_fps_ < 0.0) {
        throw std::invalid_argument("max_fps must be finite and non-negative");
    }
    interval_seconds_ = max_fps_ > 0.0 ? 1.0 / max_fps_ : 0.0;
}

PaceDecision FramePacer::ready(double now_seconds) {
    if (!is_valid_seconds(now_seconds)) {
        throw std::invalid_argument("pacer time must be finite and non-negative");
    }
    if (max_fps_ <= 0.0 || !started_) {
        return PaceDecision{true, 0.0, false};
    }

    const bool late = now_seconds > next_deadline_ + kEpsilon;
    if (now_seconds + kEpsilon >= next_deadline_) {
        return PaceDecision{true, 0.0, late};
    }

    return PaceDecision{false, next_deadline_ - now_seconds, false};
}

void FramePacer::present(double now_seconds) {
    if (!is_valid_seconds(now_seconds)) {
        throw std::invalid_argument("pacer time must be finite and non-negative");
    }
    if (max_fps_ <= 0.0) {
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
