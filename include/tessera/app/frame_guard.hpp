// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::app {

// Bounds how many consecutive frames the window driver may fail to produce before it stops.
// A swapchain that keeps refusing images (lost device, a persistent OutOfDate, or both frames in
// flight still busy) must not spin the UI thread forever, and a bounded run (`--frames N`) has to
// terminate even when no frame can be rendered. Deterministic: a counter, never a clock.
class FrameFailureGuard {
public:
    static constexpr std::uint32_t kDefaultLimit = 8;

    explicit constexpr FrameFailureGuard(
        std::uint32_t limit = kDefaultLimit) noexcept
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
    void succeeded() noexcept { consecutive_ = 0; }

    [[nodiscard]] constexpr std::uint32_t consecutive() const noexcept { return consecutive_; }
    [[nodiscard]] constexpr std::uint32_t limit() const noexcept { return limit_; }

private:
    std::uint32_t limit_ = kDefaultLimit;
    std::uint32_t consecutive_ = 0;
};

} // namespace tessera::app
