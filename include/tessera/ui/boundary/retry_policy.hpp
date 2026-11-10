// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::ui::boundary {

// Decided auto-retry policy (docs/specs/error-boundaries-v1.md section 4.1): auto-retry is off by
// default; when it is enabled the backoff is a fixed 250 ms schedule that doubles per retry and is
// capped at 4 s, for at most five retries. Exhausting the budget is the loop guard: the boundary
// reports TESSERA_BOUNDARY_LOOP and stops retrying until it is reset.
inline constexpr std::uint32_t kMaxAutoRetries = 5;
inline constexpr double kBaseBackoffSeconds = 0.25;
inline constexpr double kMaxBackoffSeconds = 4.0;

enum class RetryState : std::uint8_t {
    Idle,     // no retry pending; the guarded work is not running/scheduled
    Waiting,  // a retry is scheduled at deadline_seconds
    Ready,    // the deadline passed: run the guarded work again
    Disabled, // auto-retry is off for this boundary (max_auto_retries == 0 or budget exhausted)
};

struct RetryDecision {
    RetryState state = RetryState::Idle;
    // 1-based ordinal of the retry this decision refers to; 0 when nothing is pending.
    std::uint32_t retry = 0;
    // Backoff inserted before that retry: exactly 250/500/1000/2000/4000 ms by ordinal.
    double delay_seconds = 0.0;
    // Deadline in the caller's injected fixed-step seconds; never produced from a clock read.
    double deadline_seconds = 0.0;
    // The retry budget is exhausted: report TESSERA_BOUNDARY_LOOP and keep the fallback.
    bool loop = false;
};

// Pure deterministic retry state machine. It never reads a clock and never sleeps: the caller
// passes fixed-step time in, and the machine only compares it with the deadline it computed.
class RetryPolicy {
public:
    // `max_auto_retries` is clamped to kMaxAutoRetries; 0 disables auto-retry (the default).
    explicit RetryPolicy(std::uint32_t max_auto_retries = 0);

    // Called once per catch. Returns Waiting with the next backoff, or Disabled with `loop` set
    // when the budget is spent. Calling it while a retry is still Waiting consumes another retry,
    // so a misbehaving caller cannot spin without spending the budget.
    [[nodiscard]] RetryDecision on_failure(double now_seconds);

    // Waiting -> Ready once `now_seconds` reaches the deadline; otherwise reports the pending wait.
    [[nodiscard]] RetryDecision advance(double now_seconds);

    // Clears the budget after a successful run or an explicit reset. Required before a boundary
    // that hit the loop guard can auto-retry again.
    void reset();

    [[nodiscard]] std::uint32_t max_auto_retries() const {
        return max_auto_retries_;
    }
    [[nodiscard]] std::uint32_t used() const {
        return used_;
    }
    [[nodiscard]] bool enabled() const {
        return max_auto_retries_ != 0;
    }
    [[nodiscard]] bool loop() const {
        return loop_;
    }
    [[nodiscard]] RetryState state() const {
        return state_;
    }
    [[nodiscard]] RetryDecision pending() const;

private:
    std::uint32_t max_auto_retries_ = 0;
    std::uint32_t used_ = 0;
    RetryState state_ = RetryState::Idle;
    double delay_seconds_ = 0.0;
    double deadline_seconds_ = 0.0;
    bool loop_ = false;
};

// The capped schedule, exposed so tests, docs, and diagnostics agree on one definition. Ordinal 0
// means "no retry" and returns 0.
[[nodiscard]] double backoff_seconds(std::uint32_t retry_ordinal);

} // namespace tessera::ui::boundary
