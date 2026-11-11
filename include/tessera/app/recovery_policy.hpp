// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

namespace tessera::app {

// Attempt budget of one WindowHost::run(): at most this many device-loss recovery attempts before
// the run gives up (stats.stalled is set and stats.device_state reports Lost). Explicit rather than
// "retry forever": one attempt tears down and rebuilds the swapchain, the 2D renderer, the glyph
// atlas and, when the backend's in-place recovery is already spent, the device itself, and a driver
// reset that keeps failing is not a transient condition. The budget is per run(), not per loss, so
// a device that keeps dropping frames cannot keep the loop alive indefinitely.
inline constexpr std::uint32_t kMaxDeviceRecoveryAttempts = 3;

// What the window driver does next for one device-loss observation.
enum class RecoveryAction {
    // Nothing to do this iteration.
    None,
    // Release every RHI object of the lost generation, then let the runtime attempt recovery.
    Release,
    // Recreate the backend device from scratch and try again (the in-place attempt is spent).
    Rebuild,
    // The attempt budget is exhausted: stop the run and report a stall.
    GiveUp,
};

// Caller-side device-loss policy of the window driver (docs/design/window-resize.md section 7).
//
// A loss is observed from two places: the runtime reports the device blocked (its
// DeviceRecoveryController owns the backend's single bounded in-place `Device::recover()`), and a
// frame can fail with a device-lost error. Both funnel through this policy, which decides what the
// driver does next and charges the attempt budget; the device/swapchain work stays in WindowHost.
//
// State machine:
//   Ready      -- a loss is observed: one attempt is charged and the caller must release its
//                 lost-generation RHI objects (Release) so the backend's probe can succeed.
//   Releasing  -- still blocked while the runtime attempts in place: nothing to do (None).
//   Releasing  -- the in-place attempt succeeded (on_recovered): Ready again, the caller recreates
//                 its presentation stack.
//   Releasing  -- the attempt failed (on_failed): charge one attempt and rebuild the device
//                 (Rebuild), or give up when the budget is spent (GiveUp).
//   Failed     -- every later observation reports GiveUp so the caller cannot re-enter recovery.
//
// Deterministic: counters and enumerators only, no clock, so the host's give-up path is testable
// without a GPU.
class RecoveryPolicy {
public:
    explicit constexpr RecoveryPolicy(std::uint32_t budget = kMaxDeviceRecoveryAttempts) noexcept
        : budget_(budget) {}

    // The runtime reports the device lost/blocked (or a frame reported a device-lost error).
    // Release is returned once per loss; a repeated blocked observation while the backend is still
    // attempting returns None so the caller does not release twice.
    constexpr RecoveryAction on_blocked() noexcept {
        if (state_ == State::Failed) {
            return RecoveryAction::GiveUp;
        }
        if (attempts_ >= budget_) {
            state_ = State::Failed;
            return RecoveryAction::GiveUp;
        }
        if (state_ == State::Releasing) {
            return RecoveryAction::None;
        }
        ++attempts_;
        state_ = State::Releasing;
        return RecoveryAction::Release;
    }

    // The runtime reported a successful in-place recovery; the caller recreates its presentation
    // stack and the next frame is a full repaint.
    constexpr void on_recovered() noexcept {
        ++recoveries_;
        state_ = State::Ready;
    }

    // The recovery attempt failed (the backend's in-place `recover()`, or the caller's rebuild).
    // The rebuild is itself an attempt, so a persistent loss stops after the budget is spent.
    constexpr RecoveryAction on_failed() noexcept {
        if (state_ == State::Failed) {
            return RecoveryAction::GiveUp;
        }
        if (attempts_ >= budget_) {
            state_ = State::Failed;
            return RecoveryAction::GiveUp;
        }
        ++attempts_;
        // A rebuilt device is healthy again: the next loss starts a fresh cycle and is charged
        // then.
        state_ = State::Ready;
        return RecoveryAction::Rebuild;
    }

    // Attempts charged so far in this run() (an in-place cycle and each rebuild).
    [[nodiscard]] constexpr std::uint32_t attempts() const noexcept {
        return attempts_;
    }
    // Successful recoveries observed so far (each recreated the presentation stack).
    [[nodiscard]] constexpr std::uint32_t recoveries() const noexcept {
        return recoveries_;
    }
    [[nodiscard]] constexpr std::uint32_t budget() const noexcept {
        return budget_;
    }
    // True once every later observation reports GiveUp.
    [[nodiscard]] constexpr bool exhausted() const noexcept {
        return state_ == State::Failed;
    }
    // True while a loss is being recovered (an attempt is charged and unfinished).
    [[nodiscard]] constexpr bool lost() const noexcept {
        return state_ == State::Releasing;
    }

private:
    enum class State { Ready, Releasing, Failed };

    std::uint32_t budget_ = kMaxDeviceRecoveryAttempts;
    std::uint32_t attempts_ = 0;
    std::uint32_t recoveries_ = 0;
    State state_ = State::Ready;
};

} // namespace tessera::app
