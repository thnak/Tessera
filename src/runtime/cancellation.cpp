// SPDX-License-Identifier: MIT
//
// Cancellation (issue #845): registration slots, the linked-source link, and `cancel_after`. The
// shared state lives in `cancellation_state.hpp`, so this file is the behaviour on top of it.

#include "cancellation_state.hpp"

#include <cmath>
#include <optional>
#include <utility>

namespace tessera::runtime {

detail::CancellationState::~CancellationState() {
    if (has_timer && timer_queue != nullptr) {
        timer_queue->cancel(timer_id);
    }
}

namespace detail {

void cancel_state(const std::shared_ptr<detail::CancellationState>& state, CancelReason reason) {
    if (state == nullptr) {
        return;
    }
    if (state->cancelled.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    state->reason.store(static_cast<std::uint8_t>(reason), std::memory_order_release);

    for (std::size_t index = 0;; ++index) {
        std::function<void()> callback;
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            if (index >= state->slots.size()) {
                break;
            }
            CancellationSlot& entry = state->slots[index];
            if (!entry.active) {
                continue;
            }
            const std::uint32_t generation = entry.generation;
            callback = std::move(entry.callback);
            entry.active = false;
            entry.callback = nullptr;
            ++entry.generation;
            if (state->active_slots > 0) {
                --state->active_slots;
            }
            state->running = true;
            state->running_slot = static_cast<std::uint32_t>(index);
            state->running_generation = generation;
            state->running_thread = std::this_thread::get_id();
        }

        if (callback) {
            callback();
        }

        {
            std::lock_guard<std::mutex> lock(state->running_done_mutex);
            state->running = false;
        }
        state->running_done.notify_all();
    }
}

// Registers or, if already cancelled, runs inline. Returns the slot handle for an active slot.
bool register_cancel_callback(const std::shared_ptr<CancellationState>& state,
                              std::function<void()> callback, std::uint32_t& slot,
                              std::uint32_t& generation) {
    if (state == nullptr || !callback) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->cancelled.load(std::memory_order_acquire)) {
            CancellationSlot entry;
            entry.callback = std::move(callback);
            entry.generation = 1;
            entry.active = true;
            state->slots.push_back(std::move(entry));
            ++state->active_slots;
            slot = static_cast<std::uint32_t>(state->slots.size() - 1);
            generation = state->slots.back().generation;
            return true;
        }
    }
    callback();
    return false;
}

// Removes a slot and, if `cancel()` is running that slot's callback on another thread right now,
// waits for it to return. Re-entrant unregistration from inside the callback skips the wait.
void unregister(const std::shared_ptr<CancellationState>& state, std::uint32_t slot,
                std::uint32_t generation) {
    if (state == nullptr) {
        return;
    }
    bool wait_for_callback = false;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        const bool is_running = state->running && state->running_slot == slot &&
                                state->running_generation == generation;
        if (is_running && state->running_thread != std::this_thread::get_id()) {
            wait_for_callback = true;
        }
        if (slot < state->slots.size() && state->slots[slot].generation == generation &&
            state->slots[slot].active) {
            state->slots[slot].active = false;
            state->slots[slot].callback = nullptr;
            ++state->slots[slot].generation;
            if (state->active_slots > 0) {
                --state->active_slots;
            }
        }
    }
    if (wait_for_callback) {
        std::unique_lock<std::mutex> done(state->running_done_mutex);
        state->running_done.wait(done, [&] { return !state->running; });
    }
}

} // namespace detail

CancellationToken CancellationSource::token() const {
    return CancellationToken{state_};
}

bool CancellationToken::cancelled() const {
    return state_ != nullptr && state_->cancelled.load(std::memory_order_acquire);
}

std::optional<CancelReason> CancellationToken::reason() const {
    if (!cancelled()) {
        return std::nullopt;
    }
    return static_cast<CancelReason>(state_->reason.load(std::memory_order_acquire));
}

CancellationRegistration CancellationToken::on_cancel(std::function<void()> callback) const {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    if (register_cancel_callback(state_, std::move(callback), slot, generation)) {
        return CancellationRegistration{state_, slot, generation};
    }
    return {};
}

std::size_t CancellationToken::registration_count() const {
    if (state_ == nullptr) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->active_slots;
}

CancellationRegistration::CancellationRegistration(std::shared_ptr<detail::CancellationState> state,
                                                   std::uint32_t slot,
                                                   std::uint32_t generation) noexcept
    : state_(std::move(state)),
      slot_(slot),
      generation_(generation) {}

CancellationRegistration::CancellationRegistration(CancellationRegistration&& other) noexcept
    : state_(std::move(other.state_)),
      slot_(other.slot_),
      generation_(other.generation_) {
    other.state_ = nullptr;
}

CancellationRegistration&
CancellationRegistration::operator=(CancellationRegistration&& other) noexcept {
    if (this != &other) {
        reset();
        state_ = std::move(other.state_);
        slot_ = other.slot_;
        generation_ = other.generation_;
        other.state_ = nullptr;
    }
    return *this;
}

CancellationRegistration::~CancellationRegistration() {
    reset();
}

void CancellationRegistration::reset() noexcept {
    if (state_ != nullptr) {
        unregister(state_, slot_, generation_);
        state_ = nullptr;
    }
}

CancellationSource::CancellationSource()
    : state_(std::make_shared<detail::CancellationState>()) {}

void CancellationSource::cancel(CancelReason reason) {
    cancel_state(state_, reason);
}

bool CancellationSource::cancelled() const {
    return state_->cancelled.load(std::memory_order_acquire);
}

CancellationRegistration CancellationSource::on_cancel(std::function<void()> callback) const {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;
    if (register_cancel_callback(state_, std::move(callback), slot, generation)) {
        return CancellationRegistration{state_, slot, generation};
    }
    return {};
}

CancellationSource CancellationSource::linked(std::span<const CancellationToken> parents) {
    CancellationSource linked;
    for (const CancellationToken& parent : parents) {
        if (parent.cancelled()) {
            linked.cancel(CancelReason::Parent);
            return linked;
        }
    }
    for (const CancellationToken& parent : parents) {
        // The child holds its own token as the callback's strong reference and its state weakly.
        // That is the only reference cycle the link can form, and it is harmless: it is reachable
        // only from the child state alive, so it dies with the last outside handle to the child,
        // and destroying the child runs the registration destructors that unlink the parents.
        const std::weak_ptr<detail::CancellationState> weak_child = linked.state_;
        CancellationRegistration registration = parent.on_cancel([weak_child] {
            if (const std::shared_ptr<detail::CancellationState> child = weak_child.lock()) {
                cancel_state(child, CancelReason::Parent);
            }
        });
        std::lock_guard<std::mutex> lock(linked.state_->mutex);
        linked.state_->link_registrations.push_back(std::move(registration));
    }
    return linked;
}

bool CancellationSource::cancel_after(Clock& clock, TimerQueue& timers, double seconds) {
    if (cancelled()) {
        return false;
    }
    if (!std::isfinite(seconds) || seconds < 0.0) {
        return false;
    }
    // A second call moves the deadline: drop the outstanding timer first. A stale id is harmless
    // (the queue matches by generation), so the cancel cannot hit a reused slot.
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (state_->has_timer && state_->timer_queue != nullptr) {
            state_->timer_queue->cancel(state_->timer_id);
        }
        state_->has_timer = false;
    }

    const std::weak_ptr<detail::CancellationState> weak = state_;
    const std::optional<TimerId> id =
        timers.schedule_one_shot(clock.now_seconds() + seconds, [weak] {
            if (const std::shared_ptr<detail::CancellationState> state = weak.lock()) {
                cancel_state(state, CancelReason::Timeout);
            }
        });
    if (!id) {
        return false;
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->cancelled.load(std::memory_order_acquire)) {
        // A concurrent `cancel()` won the race: the deadline is moot.
        timers.cancel(*id);
        return false;
    }
    state_->timer_queue = &timers;
    state_->timer_id = *id;
    state_->has_timer = true;
    return true;
}

} // namespace tessera::runtime
