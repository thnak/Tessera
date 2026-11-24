// SPDX-License-Identifier: MIT
//
// Private to src/runtime: the state behind a `CancellationSource` and the helpers that mutate it.
// Not installed; only the cancellation translation units include it. The public handles
// (`CancellationToken`, `CancellationRegistration`, `CancellationSource`) forward-declare the state
// in `tessera/runtime/async.hpp`, so this definition never reaches a consumer.

#pragma once

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/runtime/timer_queue.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace tessera::runtime::detail {

// One registered callback. Slots are stable while a callback runs: unregistering is O(1) by slot
// index plus generation, and the vector is never reshuffled under a running `cancel()`.
struct CancellationSlot {
    std::function<void()> callback;
    std::uint32_t generation = 0;
    bool active = false;
};

// Shared by every handle to one source. The cancelled flag and reason are atomic so a child can
// poll them from any thread at any time; everything else is under `mutex`.
struct CancellationState {
    std::atomic<bool> cancelled{false};
    std::atomic<std::uint8_t> reason{0};

    std::mutex mutex;
    std::vector<CancellationSlot> slots;
    std::size_t active_slots = 0;

    // Set while a callback from `slots[running_slot]` runs, so a registration destroyed from
    // another thread waits for it (.NET's Dispose guarantee). `running_generation` is the slot's
    // generation as the registration saw it, which is the one the registration still holds.
    bool running = false;
    std::uint32_t running_slot = 0;
    std::uint32_t running_generation = 0;
    std::thread::id running_thread;
    std::mutex running_done_mutex;
    std::condition_variable running_done;

    // A this-source `cancel_after` deadline: the timer queue that owns the timer, its id, and
    // whether one is outstanding. Removing the timer in the destructor is what makes "destroying
    // the source removes the timer" true.
    TimerQueue* timer_queue = nullptr;
    TimerId timer_id{};
    bool has_timer = false;

    // Set by the `cancel_after` timer callback right before it cancels, so an awaiter racing a
    // deadline (`timeout`) can tell "the deadline elapsed" from "someone cancelled the source".
    // Atomic because the timer callback runs on the drain thread while other threads may observe
    // the source.
    std::atomic<bool> deadline_fired{false};

    // Registrations a `linked()` child holds on its parents. Destroying the child drops them, so a
    // long-lived parent token never accumulates dead callbacks.
    std::vector<CancellationRegistration> link_registrations;

    ~CancellationState();
};

// Cancels `state` once with `reason` and runs every registered callback on this thread.
void cancel_state(const std::shared_ptr<CancellationState>& state, CancelReason reason);

// Registers or, if already cancelled, runs inline. Returns false when the callback ran inline or
// there was nothing to register; `slot`/`generation` identify an active slot for a registration.
bool register_cancel_callback(const std::shared_ptr<CancellationState>& state,
                              std::function<void()> callback, std::uint32_t& slot,
                              std::uint32_t& generation);

// Removes a slot and, if `cancel()` is running that slot's callback on another thread right now,
// waits for it to return. Re-entrant unregistration from inside the callback skips the wait.
void unregister(const std::shared_ptr<CancellationState>& state, std::uint32_t slot,
                std::uint32_t generation);

} // namespace tessera::runtime::detail
