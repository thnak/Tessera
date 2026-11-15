// SPDX-License-Identifier: MIT

#include "tessera/runtime/timer_queue.hpp"

#include <cmath>
#include <stdexcept>
#include <utility>

namespace tessera::runtime {

namespace {

bool is_finite_positive(double value) {
    return std::isfinite(value) && value > 0.0;
}

// Clears the re-entrancy latch even if a callback throws. A timer callback belongs to a layer whose
// boundary converts exceptions (ADR-025), but a queue that latched itself shut on the way out would
// silently stop firing every later timer, so the reset is unconditional.
struct FiringGuard {
    bool& latch;
    explicit FiringGuard(bool& target)
        : latch(target) {
        latch = true;
    }
    ~FiringGuard() {
        latch = false;
    }
    FiringGuard(const FiringGuard&) = delete;
    FiringGuard& operator=(const FiringGuard&) = delete;
};

} // namespace

TimerQueue::TimerQueue(std::size_t capacity)
    : slots_(std::make_unique<Slot[]>(capacity)),
      capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("timer queue capacity must be greater than zero");
    }
}

TimerQueue::~TimerQueue() = default;

std::optional<TimerId> TimerQueue::schedule_one_shot(double deadline_seconds,
                                                     std::function<void()> callback) {
    return schedule(deadline_seconds, 0.0, std::move(callback));
}

std::optional<TimerId> TimerQueue::schedule_periodic(double deadline_seconds, double period_seconds,
                                                     std::function<void()> callback) {
    if (!is_finite_positive(period_seconds)) {
        throw std::invalid_argument("timer period must be finite and positive");
    }
    return schedule(deadline_seconds, period_seconds, std::move(callback));
}

std::optional<TimerId> TimerQueue::schedule(double deadline_seconds, double period_seconds,
                                            std::function<void()> callback) {
    if (!std::isfinite(deadline_seconds)) {
        throw std::invalid_argument("timer deadline must be finite");
    }
    if (!callback) {
        throw std::invalid_argument("timer callback must not be empty");
    }

    for (std::size_t index = 0; index < capacity_; ++index) {
        Slot& slot = slots_[index];
        if (slot.active) {
            continue;
        }
        slot.deadline = deadline_seconds;
        slot.period = period_seconds;
        slot.sequence = sequence_++;
        slot.active = true;
        slot.callback = std::move(callback);
        ++active_;
        return TimerId{static_cast<std::uint32_t>(index), slot.generation};
    }
    return std::nullopt;
}

bool TimerQueue::cancel(TimerId id) {
    if (id.slot >= capacity_) {
        return false;
    }
    Slot& slot = slots_[id.slot];
    if (!slot.active || slot.generation != id.generation) {
        return false;
    }
    release(slot);
    return true;
}

std::size_t TimerQueue::clear() {
    std::size_t cleared = 0;
    for (std::size_t index = 0; index < capacity_; ++index) {
        if (slots_[index].active) {
            release(slots_[index]);
            ++cleared;
        }
    }
    return cleared;
}

std::size_t TimerQueue::fire_due(double now_seconds) {
    if (!std::isfinite(now_seconds) || firing_) {
        // A non-finite instant would fire nothing sensible; a nested call would let a callback
        // fire the timer that is currently running. Both are no-ops, and neither throws here:
        // this runs on the frame path.
        return 0;
    }
    FiringGuard guard(firing_);
    std::size_t fired = 0;
    while (Slot* slot = earliest_due(now_seconds)) {
        const std::uint32_t generation = slot->generation;
        // Move the callback out before it runs: a callback that schedules a new timer reuses this
        // slot (one-shot) or leaves it in place (periodic), and either way must not be able to
        // overwrite the std::function currently executing.
        std::function<void()> callback = std::move(slot->callback);
        if (slot->period > 0.0) {
            // Coalesce missed periods: advance to the first deadline strictly after `now`.
            const double elapsed = now_seconds - slot->deadline;
            const double missed = std::floor(elapsed / slot->period) + 1.0;
            slot->deadline += missed * slot->period;
        } else {
            release(*slot);
        }
        ++fires_;
        ++fired;
        callback();
        // `callback` may have cancelled this timer (periodic) or reused the slot; the generation
        // check makes the next iteration see only what is still armed.
        if (slot->active && slot->generation == generation && slot->period > 0.0) {
            slot->callback = std::move(callback);
        }
    }
    return fired;
}

double TimerQueue::next_deadline_seconds() const noexcept {
    double earliest = kNoDeadline;
    for (std::size_t index = 0; index < capacity_; ++index) {
        const Slot& slot = slots_[index];
        if (slot.active && slot.deadline < earliest) {
            earliest = slot.deadline;
        }
    }
    return earliest;
}

std::size_t TimerQueue::size() const noexcept {
    return active_;
}

void TimerQueue::release(Slot& slot) noexcept {
    slot.active = false;
    slot.period = 0.0;
    slot.sequence = 0;
    slot.callback = nullptr;
    // Bumping the generation on release is what makes a stale handle a no-op rather than a hit on
    // whichever timer takes the slot next.
    ++slot.generation;
    if (active_ > 0) {
        --active_;
    }
}

TimerQueue::Slot* TimerQueue::earliest_due(double now_seconds) noexcept {
    Slot* earliest = nullptr;
    for (std::size_t index = 0; index < capacity_; ++index) {
        Slot& slot = slots_[index];
        if (!slot.active || slot.deadline > now_seconds) {
            continue;
        }
        if (earliest == nullptr || slot.deadline < earliest->deadline ||
            (slot.deadline == earliest->deadline && slot.sequence < earliest->sequence)) {
            earliest = &slot;
        }
    }
    return earliest;
}

} // namespace tessera::runtime
