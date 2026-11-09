// SPDX-License-Identifier: MIT

#include "tessera/runtime/device_recovery.hpp"

namespace tessera::runtime {

DeviceRecoveryController::DeviceRecoveryController(rhi::Device& device)
    : device_(&device) {}

DeviceRecoveryUpdate DeviceRecoveryController::tick() {
    DeviceRecoveryUpdate update;
    const bool lost = device_->status() == rhi::DeviceStatus::Lost;

    if (!lost) {
        // A healthy device clears the failure latch: a later loss is a new loss and gets its own
        // one-shot attempt.
        failed_for_loss_ = false;
        state_ = DeviceRecoveryState::Ready;
        update.state = state_;
        update.generation = generation_;
        return update;
    }

    update.loss_observed = true;
    if (state_ == DeviceRecoveryState::Ready) {
        ++losses_;
    }

    if (failed_for_loss_) {
        // D7 one-shot policy: this loss already consumed its attempt. Stay Failed without touching
        // the device until it is observed Ready again.
        state_ = DeviceRecoveryState::Failed;
        update.state = state_;
        update.failed = true;
        update.generation = generation_;
        return update;
    }

    // One bounded attempt at most per tick. The backend returns a value and never blocks the tick
    // beyond its own bounded device creation.
    state_ = DeviceRecoveryState::Recovering;
    ++attempts_;
    update.attempt_made = true;
    const auto result = device_->recover();

    if (result.has_value()) {
        ++generation_;
        state_ = DeviceRecoveryState::Ready;
        update.recovered = true;
        update.full_frame_required = true;
    } else {
        switch (result.error()) {
        case rhi::DeviceRecoveryError::ResourcesOutstanding:
            // The caller still owns lost-generation RHI objects. Retry cheaply on a later tick;
            // this does not consume the one-shot attempt.
            state_ = DeviceRecoveryState::WaitingForCaller;
            update.resources_outstanding = true;
            break;
        case rhi::DeviceRecoveryError::AlreadyAttempted:
        case rhi::DeviceRecoveryError::DeviceCreationFailed:
            state_ = DeviceRecoveryState::Failed;
            failed_for_loss_ = true;
            update.failed = true;
            break;
        case rhi::DeviceRecoveryError::NotLost:
            // The device became healthy between the status check and recover(); treat the tick as a
            // fresh generation so callers repaint.
            ++generation_;
            state_ = DeviceRecoveryState::Ready;
            update.recovered = true;
            update.full_frame_required = true;
            break;
        case rhi::DeviceRecoveryError::NotSupported:
            // A backend without a recovery path cannot recover a reported loss.
            state_ = DeviceRecoveryState::Failed;
            failed_for_loss_ = true;
            update.failed = true;
            break;
        }
    }

    update.state = state_;
    update.generation = generation_;
    return update;
}

} // namespace tessera::runtime
