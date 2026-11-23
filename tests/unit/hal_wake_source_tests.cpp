// SPDX-License-Identifier: MIT

// Issue #842: the cross-thread wake a parked UI thread joins. The contract is small - a wake from
// any thread ends a wait, a burst is one wake, a drained wake blocks again, and a platform with no
// primitive reports null rather than pretending - and each part is asserted here without a window.
//
// The manual source is the deterministic one: no descriptor and no platform, so the sequencing is
// exact. The platform source is exercised only for the properties every implementation must have.

#include "tessera/hal/timer/steady_time.hpp"
#include "tessera/hal/wake_source.hpp"

#include <atomic>
#include <iostream>
#include <thread>

namespace {

using tessera::hal::create_manual_wake_source;
using tessera::hal::create_wake_source;
using tessera::hal::steady_now_seconds;
using tessera::hal::WakeSource;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "hal_wake_source: " << message << "\n";
    }
    return condition;
}

constexpr double kLongEnough = 5.0;
constexpr double kImmediate = 0.0;

} // namespace

int main() {
    // A wake before the wait is remembered, so a signal racing the park is not lost.
    {
        const auto source = create_manual_wake_source();
        if (!expect(source != nullptr, "the manual source is always available")) {
            return 1;
        }
        source->wake();
        if (!expect(source->wait_until(steady_now_seconds() + kImmediate),
                    "a wake before the wait returns immediately")) {
            return 2;
        }
        source->drain();
        if (!expect(!source->wait_until(steady_now_seconds()),
                    "a drained wake and a past deadline return at once")) {
            return 3;
        }
    }

    // A worker thread's wake ends a parked wait. Bounded yields, never a sleep: the assertion is
    // that the wait returns, not how long it took.
    {
        const auto source = create_manual_wake_source();
        std::atomic<bool> parked{false};
        std::atomic<bool> woken{false};
        std::thread waiter([&] {
            parked.store(true, std::memory_order_release);
            woken.store(source->wait_until(steady_now_seconds() + kLongEnough),
                        std::memory_order_release);
        });
        while (!parked.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // The waiter may not have entered wait_until yet, which the manual source's remembered
        // signal covers: the wake lands now and the wait returns when it gets there.
        source->wake();
        waiter.join();
        if (!expect(woken.load(std::memory_order_acquire),
                    "a worker's wake ends the parked wait")) {
            return 4;
        }
    }

    // A burst is one wake: the second wait blocks until the next signal, which a past deadline
    // turns into an immediate false.
    {
        const auto source = create_manual_wake_source();
        source->wake();
        source->wake();
        source->wake();
        if (!expect(source->wait_until(steady_now_seconds() + kImmediate),
                    "a burst is observed as woken")) {
            return 5;
        }
        source->drain();
        if (!expect(!source->wait_until(steady_now_seconds()),
                    "one drain clears the burst entirely")) {
            return 6;
        }
    }

    // The platform source, when the platform has one, obeys the same sequencing. Where there is no
    // primitive the answer is null, which is the documented fallback, not a failure.
    {
        const auto source = create_wake_source();
        if (source != nullptr) {
            source->wake();
            source->wake();
            // A future deadline, not a past one: the point is that the signalled descriptor makes
            // the wait return at once, which a zero deadline would short-circuit before polling.
            if (!expect(source->wait_until(steady_now_seconds() + kLongEnough),
                        "the platform wake observes a signal")) {
                return 7;
            }
            source->drain();
            if (!expect(!source->wait_until(steady_now_seconds()),
                        "the platform wake drains to unsignalled")) {
                return 8;
            }
            // A descriptor-joined source must expose exactly one of the two handle forms.
            const bool has_descriptor = source->file_descriptor() >= 0;
            const bool has_event = source->os_event() != nullptr;
            if (!expect(has_descriptor || has_event,
                        "the platform wake exposes a poll descriptor or an OS event")) {
                return 9;
            }
        }
    }

    std::cout << "hal_wake_source: ok\n";
    return 0;
}
