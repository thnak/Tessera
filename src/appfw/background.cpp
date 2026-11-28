// SPDX-License-Identifier: MIT
//
// `BackgroundCoordinator` (issue #453). See include/tessera/appfw/background.hpp.

#include "tessera/appfw/background.hpp"

#include <algorithm>
#include <utility>

namespace tessera::appfw {

BackgroundCoordinator::BackgroundCoordinator(runtime::TimerQueue* timers, std::size_t capacity)
    : timers_(timers),
      slots_(std::make_unique<Slot[]>(std::max<std::size_t>(capacity, 1))),
      capacity_(std::max<std::size_t>(capacity, 1)) {}

std::optional<std::uint32_t>
BackgroundCoordinator::register_scope(BackgroundRegistration registration) {
    if (registration.scope == nullptr || count_ == capacity_) {
        ++refused_;
        return std::nullopt;
    }
    for (std::size_t index = 0; index < capacity_; ++index) {
        Slot& slot = slots_[index];
        if (!slot.used) {
            slot.used = true;
            slot.id = next_id_++;
            slot.registration = std::move(registration);
            ++count_;
            return slot.id;
        }
    }
    ++refused_;
    return std::nullopt;
}

bool BackgroundCoordinator::unregister(std::uint32_t id) {
    for (std::size_t index = 0; index < capacity_; ++index) {
        Slot& slot = slots_[index];
        if (slot.used && slot.id == id) {
            slot.used = false;
            slot.registration = {};
            --count_;
            return true;
        }
    }
    return false;
}

void BackgroundCoordinator::on_app_state(AppState state) {
    switch (state) {
    case AppState::Hidden:
        if (suspended_) {
            leave_suspended();
        }
        enter_background();
        break;
    case AppState::Suspended:
        enter_background();
        enter_suspended();
        break;
    case AppState::Launching:
    case AppState::Active:
    case AppState::Inactive:
        if (suspended_) {
            leave_suspended();
        }
        backgrounded_ = false;
        break;
    case AppState::Terminating:
    case AppState::Terminated:
        // The shutdown sequencer owns closing scopes; a late checkpoint here would race it.
        break;
    }
}

void BackgroundCoordinator::enter_background() {
    if (backgrounded_) {
        return;
    }
    backgrounded_ = true;
    for (std::size_t index = 0; index < capacity_; ++index) {
        Slot& slot = slots_[index];
        if (slot.used && slot.registration.checkpoint) {
            slot.registration.checkpoint();
            ++checkpoints_run_;
        }
    }
}

void BackgroundCoordinator::enter_suspended() {
    if (suspended_) {
        return;
    }
    suspended_ = true;
    if (timers_ != nullptr) {
        timers_->set_paused(true);
    }
    for (std::size_t index = 0; index < capacity_; ++index) {
        Slot& slot = slots_[index];
        if (slot.used && slot.registration.policy != BackgroundPolicy::Continue &&
            !slot.registration.scope->cancelled()) {
            slot.registration.scope->cancel();
            ++scopes_cancelled_;
        }
    }
}

void BackgroundCoordinator::leave_suspended() {
    suspended_ = false;
    if (timers_ != nullptr) {
        timers_->set_paused(false);
    }
}

} // namespace tessera::appfw
