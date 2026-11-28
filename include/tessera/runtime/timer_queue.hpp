// SPDX-License-Identifier: MIT
//
// The layer-0 timer wake source (ADR-050 section 5 item 2): a bounded queue of one-shot and
// periodic timers whose earliest deadline the loop exposes so an idle app parks until it instead of
// polling.
//
// Why this lives in `src/runtime/` and not in `appfw`: ADR-050 section 1 rule 1 says layer 0 gains
// *seams, not concepts*. A timer is a wake source the loop has to schedule against, so the queue is
// layer 0; what the timer is *for* (a service's retry, a component's debounce) stays above it.
// There is no `appfw` type or name in this header, and none may appear here.
//
// Determinism: every deadline is an absolute instant on the injected clock, so a `ManualClock`
// drives the queue exactly. Firing order is (deadline, registration sequence), never insertion into
// a container or address order, and `fire_due` takes the instant as a parameter instead of reading
// a clock: it never sleeps, never spins and never consults wall time.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>

namespace tessera::runtime {

// No timer is scheduled. A sentinel rather than `0`, because `0` is a legal deadline (the clock's
// origin) and a loop that cannot tell "due now" from "nothing scheduled" cannot park correctly.
inline constexpr double kNoDeadline = std::numeric_limits<double>::infinity();

// Slot plus generation. The generation makes a stale handle harmless: cancelling a timer that
// already fired, or one whose slot was reused by a later timer, is a no-op instead of a hit on an
// unrelated timer.
struct TimerId {
    std::uint32_t slot = 0;
    std::uint32_t generation = 0;

    [[nodiscard]] friend constexpr bool operator==(TimerId, TimerId) = default;
};

// Fixed-capacity timer queue: storage is allocated once at construction, scheduling reuses an
// inactive slot, and firing allocates nothing on the one-shot path. A full queue refuses a new
// timer (`std::nullopt`) instead of growing; the caller decides whether that is a bug or a
// backpressure signal.
class TimerQueue {
public:
    static constexpr std::size_t kDefaultCapacity = 64;

    explicit TimerQueue(std::size_t capacity = kDefaultCapacity);
    ~TimerQueue();

    TimerQueue(const TimerQueue&) = delete;
    TimerQueue& operator=(const TimerQueue&) = delete;

    // `deadline_seconds` is absolute on the clock the owner passes to `fire_due`. Throws
    // `std::invalid_argument` for a non-finite deadline, for a periodic timer whose period is not
    // finite and positive, or for an empty callback. Scheduling is not a frame path, so an
    // exception here is a boundary error like `FramePacer`'s config validation.
    [[nodiscard]] std::optional<TimerId> schedule_one_shot(double deadline_seconds,
                                                           std::function<void()> callback);
    // The first fire is at `deadline_seconds`; later fires are at whole periods after it. A fire
    // that finds one or more periods already missed fires **once** and advances the deadline past
    // `now`, so a long parking window produces one callback, not a burst of catch-up work.
    [[nodiscard]] std::optional<TimerId> schedule_periodic(double deadline_seconds,
                                                           double period_seconds,
                                                           std::function<void()> callback);

    // False for an unknown/stale id or an already-fired timer. Cancelling a timer from inside its
    // own callback is allowed and takes effect after the callback returns.
    bool cancel(TimerId id);
    // Drops every timer and returns how many were active. A callback that calls `clear()` does not
    // resurrect the timer it belongs to.
    std::size_t clear();

    // Fires every timer whose deadline is at or before `now_seconds`, earliest deadline first and,
    // for equal deadlines, in registration order. Returns the number fired. A non-finite `now`
    // fires nothing: this is called from `RuntimeLoop::tick()`, and a broken clock must not throw
    // across the frame loop. A callback that calls `fire_due` again fires nothing (the outer call
    // owns the iteration); a callback may schedule or cancel freely.
    std::size_t fire_due(double now_seconds);

    // Pause/resume the whole queue (the `Suspended` policy, issue #453). While paused `fire_due`
    // fires nothing and `next_deadline_seconds` reports `kNoDeadline`, so a parked loop does not
    // wake for a timer it may not run. Timers keep their deadlines: on resume anything overdue
    // fires on the next `fire_due` (a periodic timer coalesces to one fire, as after any long gap).
    void set_paused(bool paused) noexcept {
        paused_ = paused;
    }
    [[nodiscard]] bool paused() const noexcept {
        return paused_;
    }

    // Earliest active deadline, or `kNoDeadline` when nothing is scheduled: what the loop joins
    // with the pacer deadline so a host parks for the nearer of the two.
    [[nodiscard]] double next_deadline_seconds() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;
    [[nodiscard]] std::size_t capacity() const noexcept {
        return capacity_;
    }
    // Cumulative fires and completed registration order, which is what an ordering test asserts
    // without inspecting private slots.
    [[nodiscard]] std::uint64_t fires() const noexcept {
        return fires_;
    }

private:
    struct Slot {
        double deadline = 0.0;
        double period = 0.0;
        std::uint64_t sequence = 0;
        std::uint32_t generation = 0;
        bool active = false;
        std::function<void()> callback;
    };

    [[nodiscard]] std::optional<TimerId> schedule(double deadline_seconds, double period_seconds,
                                                  std::function<void()> callback);
    void release(Slot& slot) noexcept;
    // The due timer with the lowest (deadline, sequence), or null.
    [[nodiscard]] Slot* earliest_due(double now_seconds) noexcept;

    std::unique_ptr<Slot[]> slots_;
    std::size_t capacity_ = 0;
    std::size_t active_ = 0;
    std::uint64_t sequence_ = 0;
    std::uint64_t fires_ = 0;
    // Guards the iteration: a callback that calls `fire_due` would otherwise mutate the slot it is
    // being invoked from.
    bool firing_ = false;
    bool paused_ = false;
};

} // namespace tessera::runtime
