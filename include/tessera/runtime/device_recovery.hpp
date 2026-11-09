// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/rendering/rhi/device.hpp"

#include <cstdint>

namespace tessera::runtime {

namespace rhi = tessera::rendering::rhi;

// Device-loss orchestration state (D7). The backend detects the loss and reports it through
// `rhi::Device::status()`/`recover()`; this controller owns the frame-boundary policy on top:
// stop presenting, let the caller release its lost-generation RHI objects, then run at most one
// bounded recovery attempt per tick and report the outcome as a value.
//
// State machine:
//   Ready             -> device healthy; frames flow normally.
//   WaitingForCaller  -> lost and recovery would be refused because caller-owned RHI objects from
//                        the lost generation are still alive. Each tick makes one cheap recover()
//                        probe; the caller must destroy its objects. This does not consume the
//                        backend's one-shot recovery budget.
//   Recovering        -> transient state while the single bounded recover() call of a tick is in
//                        progress. `tick()` always resolves it (success -> Ready, refusal ->
//                        WaitingForCaller, failure -> Failed) before returning, so the UI thread
//                        never observes it across ticks.
//   Failed            -> the one-shot recovery failed; frames stay stopped and no further attempts
//                        are made until the device is observed Ready again (an external/new
//                        recovery) and then Lost again (a new loss).
enum class DeviceRecoveryState { Ready, WaitingForCaller, Recovering, Failed };

struct DeviceRecoveryUpdate {
    DeviceRecoveryState state = DeviceRecoveryState::Ready;
    // Lost was reported by the backend on this tick.
    bool loss_observed = false;
    // Exactly one recover() call was made on this tick (never more).
    bool attempt_made = false;
    // recover() succeeded on this tick; every RHI object from the old generation is stale.
    bool recovered = false;
    // The one-shot recovery for this loss failed; no further attempts until a new loss.
    bool failed = false;
    // recover() reported ResourcesOutstanding: the caller must destroy its lost-generation objects.
    bool resources_outstanding = false;
    // True on the tick a recovery succeeded: the caller recreates the swapchain/resources and the
    // next tick must repaint a full frame (damage state is invalid).
    bool full_frame_required = false;
    // Increments once per successful recovery. Callers recreate resources when it changes.
    std::uint32_t generation = 0;
};

class DeviceRecoveryController {
public:
    explicit DeviceRecoveryController(rhi::Device& device);

    // Called once per RuntimeLoop tick. Never blocks the tick (the backend recovery is a bounded,
    // value-returning call) and makes at most one recovery attempt.
    [[nodiscard]] DeviceRecoveryUpdate tick();

    [[nodiscard]] DeviceRecoveryState state() const {
        return state_;
    }
    // True while frames/presents must not run.
    [[nodiscard]] bool recovery_required() const {
        return state_ != DeviceRecoveryState::Ready;
    }
    // Bumps once per successful recovery; compare across ticks to recreate stale resources.
    [[nodiscard]] std::uint32_t generation() const {
        return generation_;
    }
    // Total recover() calls made (diagnostics/tests).
    [[nodiscard]] std::uint32_t attempts() const {
        return attempts_;
    }
    // Total losses observed (diagnostics/tests).
    [[nodiscard]] std::uint32_t losses() const {
        return losses_;
    }

private:
    rhi::Device* device_ = nullptr;
    DeviceRecoveryState state_ = DeviceRecoveryState::Ready;
    std::uint32_t generation_ = 0;
    std::uint32_t attempts_ = 0;
    std::uint32_t losses_ = 0;
    // Latched once this loss exhausted the one-shot recovery; cleared when the device is observed
    // Ready again so the next loss gets a fresh attempt.
    bool failed_for_loss_ = false;
};

} // namespace tessera::runtime
