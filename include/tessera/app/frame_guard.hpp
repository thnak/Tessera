// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::app {

// Bounds how many consecutive frames the window driver may fail to produce before it stops.
// A swapchain that keeps refusing images (lost device, a persistent OutOfDate) must not spin the UI
// thread forever, and a bounded run (`--frames N`) has to terminate even when no frame can be
// rendered. Deterministic: a counter, never a clock.
class FrameFailureGuard {
public:
    static constexpr std::uint32_t kDefaultLimit = 8;

    explicit constexpr FrameFailureGuard(std::uint32_t limit = kDefaultLimit) noexcept
        : limit_(limit) {}

    // Records one failed attempt. Returns true once the limit is reached; the caller must stop.
    bool failed() noexcept {
        if (limit_ == 0) {
            return true;
        }
        ++consecutive_;
        return consecutive_ >= limit_;
    }

    // A produced frame resets the run of failures.
    void succeeded() noexcept {
        consecutive_ = 0;
    }

    [[nodiscard]] constexpr std::uint32_t consecutive() const noexcept {
        return consecutive_;
    }
    [[nodiscard]] constexpr std::uint32_t limit() const noexcept {
        return limit_;
    }

private:
    std::uint32_t limit_ = kDefaultLimit;
    std::uint32_t consecutive_ = 0;
};

// Bounds a run of consecutive *busy-slot* retries, which are backpressure rather than faults.
//
// A slot whose GPU work has not completed must not be waited on (ADR-021 forbids blocking the UI
// thread), so the driver retries on its bounded backoff instead. That retry cannot share
// FrameFailureGuard's budget: under real load -- several processes sharing one GPU -- a slot stays
// busy longer than a frame, and eight retries at 2 ms is only ~16 ms, less than one 60 Hz frame, so
// the fault guard would stop a run that would have painted on the next completion.
//
// It still has to be finite, because a slot that never completes must not spin the UI thread
// forever. 250 retries at 2 ms is about half a second: far past any completion a loaded machine
// produces, while still bounding the run.
class BusyRetryBudget {
public:
    static constexpr std::uint32_t kDefaultLimit = 250;

    explicit constexpr BusyRetryBudget(std::uint32_t limit = kDefaultLimit) noexcept
        : limit_(limit) {}

    // Records one busy retry. Returns true once the budget is spent; the caller must stop.
    bool retried() noexcept {
        if (limit_ == 0) {
            return true;
        }
        ++consecutive_;
        return consecutive_ >= limit_;
    }

    // A produced frame ends the run of busy retries.
    void succeeded() noexcept {
        consecutive_ = 0;
    }

    [[nodiscard]] constexpr std::uint32_t consecutive() const noexcept {
        return consecutive_;
    }
    [[nodiscard]] constexpr std::uint32_t limit() const noexcept {
        return limit_;
    }

private:
    std::uint32_t limit_ = kDefaultLimit;
    std::uint32_t consecutive_ = 0;
};

} // namespace tessera::app
