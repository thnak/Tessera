// SPDX-License-Identifier: MIT

#include "tessera/ui/boundary/retry_policy.hpp"

#include <algorithm>
#include <cmath>

namespace tessera::ui::boundary {

double backoff_seconds(std::uint32_t retry_ordinal) {
    if (retry_ordinal == 0) {
        return 0.0;
    }
    // ldexp keeps the published schedule exact: 0.25, 0.5, 1.0, 2.0, 4.0 are all binary-exact, so
    // tests can compare with == and the cap is reached without accumulated rounding.
    const int exponent = static_cast<int>(std::min(retry_ordinal - 1, kMaxAutoRetries));
    const double delay = std::ldexp(kBaseBackoffSeconds, exponent);
    return std::min(delay, kMaxBackoffSeconds);
}

RetryPolicy::RetryPolicy(std::uint32_t max_auto_retries)
    : max_auto_retries_(std::min(max_auto_retries, kMaxAutoRetries)) {}

RetryDecision RetryPolicy::on_failure(double now_seconds) {
    RetryDecision decision;
    if (max_auto_retries_ == 0) {
        decision.state = RetryState::Disabled;
        return decision;
    }
    if (used_ >= max_auto_retries_) {
        // The budget is spent; staying Disabled until reset is the loop guard. One report per
        // boundary is emitted by the caller, which is why the decision only carries the flag.
        state_ = RetryState::Disabled;
        loop_ = true;
        decision.state = RetryState::Disabled;
        decision.retry = used_;
        decision.delay_seconds = delay_seconds_;
        decision.deadline_seconds = deadline_seconds_;
        decision.loop = true;
        return decision;
    }

    ++used_;
    delay_seconds_ = backoff_seconds(used_);
    deadline_seconds_ = now_seconds + delay_seconds_;
    state_ = RetryState::Waiting;

    decision.state = RetryState::Waiting;
    decision.retry = used_;
    decision.delay_seconds = delay_seconds_;
    decision.deadline_seconds = deadline_seconds_;
    return decision;
}

RetryDecision RetryPolicy::advance(double now_seconds) {
    if (state_ == RetryState::Waiting && now_seconds >= deadline_seconds_) {
        state_ = RetryState::Ready;
    }
    return pending();
}

void RetryPolicy::reset() {
    used_ = 0;
    state_ = max_auto_retries_ == 0 ? RetryState::Disabled : RetryState::Idle;
    delay_seconds_ = 0.0;
    deadline_seconds_ = 0.0;
    loop_ = false;
}

RetryDecision RetryPolicy::pending() const {
    RetryDecision decision;
    decision.state = state_;
    decision.retry = state_ == RetryState::Idle ? 0 : used_;
    decision.delay_seconds = delay_seconds_;
    decision.deadline_seconds = deadline_seconds_;
    return decision;
}

} // namespace tessera::ui::boundary
