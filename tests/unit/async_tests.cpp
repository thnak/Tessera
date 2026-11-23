// SPDX-License-Identifier: MIT

#include "tessera/runtime/async.hpp"

#include <atomic>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {

using tessera::runtime::CancellationSource;
using tessera::runtime::CancellationToken;
using tessera::runtime::ManualExecutor;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "async: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    const CancellationToken empty;
    if (!expect(!empty.cancelled(), "default token not cancelled")) {
        return 1;
    }

    CancellationSource source;
    const CancellationToken token = source.token();
    const CancellationToken copy = token;
    if (!expect(!token.cancelled(), "token initially live") ||
        !expect(!source.cancelled(), "source initially live")) {
        return 2;
    }
    source.cancel();
    if (!expect(token.cancelled(), "token cancelled") ||
        !expect(copy.cancelled(), "copied token cancelled") ||
        !expect(source.cancelled(), "source cancelled")) {
        return 3;
    }
    source.cancel();
    if (!expect(token.cancelled(), "cancel idempotent")) {
        return 4;
    }

    CancellationSource callbacks;
    std::vector<int> callback_order;
    auto source_registration = callbacks.on_cancel([&] { callback_order.push_back(1); });
    const auto callback_token = callbacks.token();
    auto token_registration = callback_token.on_cancel([&] { callback_order.push_back(2); });
    callbacks.cancel();
    if (!expect(callback_order == std::vector<int>{1, 2}, "callbacks once in order")) {
        return 20;
    }
    callbacks.cancel();
    if (!expect(callback_order == std::vector<int>{1, 2}, "callbacks not repeated")) {
        return 21;
    }
    // The registrations were consumed by the cancel, so destroying them afterwards is a no-op.
    source_registration.reset();
    token_registration.reset();
    auto late_registration = callback_token.on_cancel([&] { callback_order.push_back(3); });
    auto late_source_registration = callbacks.on_cancel([&] { callback_order.push_back(4); });
    if (!expect(callback_order == std::vector<int>{1, 2, 3, 4}, "late callbacks immediate")) {
        return 22;
    }
    auto empty_registration = callback_token.on_cancel({});
    if (!expect(callback_order.size() == 4, "empty callback ignored")) {
        return 23;
    }

    CancellationSource nested_callbacks;
    std::vector<int> nested_callback_order;
    auto nested_registration = nested_callbacks.on_cancel([&] {
        nested_callback_order.push_back(1);
        auto inner = nested_callbacks.on_cancel([&] { nested_callback_order.push_back(2); });
        (void)inner;
    });
    nested_callbacks.cancel();
    if (!expect(nested_callback_order == std::vector<int>{1, 2},
                "callback registration after cancel")) {
        return 24;
    }

    const CancellationToken no_source;
    auto no_source_registration = no_source.on_cancel([&] { callback_order.push_back(99); });
    if (!expect(callback_order.size() == 4, "default token callback ignored") ||
        !expect(!no_source_registration.active(), "a default token yields an empty registration")) {
        return 25;
    }

    ManualExecutor executor(4);
    std::vector<int> order;
    if (!expect(executor.post([&] { order.push_back(1); }), "post one") ||
        !expect(executor.post([&] { order.push_back(2); }), "post two") ||
        !expect(executor.size() == 2, "queue size") ||
        !expect(executor.run_one(), "run one true") ||
        !expect(order.size() == 1 && order[0] == 1, "fifo first") ||
        !expect(executor.run(10) == 1, "run remaining") ||
        !expect(order.size() == 2 && order[1] == 2, "fifo second") ||
        !expect(executor.empty(), "queue drained") ||
        !expect(executor.stats().posted == 2 && executor.stats().executed == 2, "executor stats")) {
        return 5;
    }

    ManualExecutor nested(4);
    std::vector<int> nested_order;
    nested.post([&] {
        nested_order.push_back(1);
        nested.post([&] { nested_order.push_back(3); });
    });
    nested.post([&] { nested_order.push_back(2); });
    if (!expect(nested.run(3) == 3, "nested run count") ||
        !expect(nested_order == std::vector<int>{1, 2, 3}, "nested fifo")) {
        return 6;
    }

    ManualExecutor bounded(2);
    if (!expect(bounded.post([] {}), "bounded first") ||
        !expect(bounded.post([] {}), "bounded second") ||
        !expect(!bounded.post([] {}), "bounded overflow") ||
        !expect(bounded.stats().dropped == 1, "overflow stat") ||
        !expect(bounded.stats().pending == 2, "pending stat") ||
        !expect(bounded.clear() == 2, "clear count") ||
        !expect(bounded.stats().discarded == 2, "discard stat") ||
        !expect(bounded.empty(), "clear empty")) {
        return 7;
    }

    CancellationSource cancelled_after;
    const CancellationToken late = cancelled_after.token();
    ManualExecutor cancellation(4);
    if (!expect(cancellation.post([] {}, late), "post before cancel")) {
        return 8;
    }
    cancelled_after.cancel();
    if (!expect(!cancellation.run_one(), "cancelled task skipped") ||
        !expect(cancellation.stats().executed == 0, "cancelled not executed") ||
        !expect(cancellation.stats().cancelled == 1, "cancelled stat")) {
        return 9;
    }

    CancellationSource cancelled_before;
    cancelled_before.cancel();
    if (!expect(!cancellation.post([] {}, cancelled_before.token()), "post cancel rejected") ||
        !expect(cancellation.stats().cancelled == 2, "post cancel stat") ||
        !expect(!cancellation.post({}), "null task rejected") ||
        !expect(cancellation.stats().dropped == 1, "null task dropped")) {
        return 10;
    }

    ManualExecutor budgeted(2);
    int runs = 0;
    budgeted.post([&] { ++runs; });
    if (!expect(budgeted.run(0) == 0, "zero budget") ||
        !expect(runs == 0, "zero budget executes nothing") ||
        !expect(budgeted.run(1) == 1 && runs == 1, "budget executes one")) {
        return 11;
    }

    bool threw = false;
    try {
        ManualExecutor invalid(0);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!expect(threw, "zero capacity rejected")) {
        return 12;
    }

    // --- #845: RAII registration returns the list to its starting size ------------------------
    {
        CancellationSource raii_source;
        const auto raii_token = raii_source.token();
        {
            std::vector<tessera::runtime::CancellationRegistration> registrations;
            registrations.reserve(10000);
            for (int i = 0; i < 10000; ++i) {
                registrations.push_back(raii_token.on_cancel([] {}));
            }
            if (!expect(raii_token.registration_count() == 10000,
                        "ten thousand registrations are live")) {
                return 13;
            }
        }
        if (!expect(raii_token.registration_count() == 0,
                    "destroying them returns the list to zero")) {
            return 14;
        }
        int calls = 0;
        raii_source.cancel();
        if (!expect(calls == 0, "a cancelled-after-unregister callback never ran")) {
            return 15;
        }

        // `reset()` is the explicit form and is idempotent.
        CancellationSource reset_source;
        auto reset_token = reset_source.token();
        auto registration = reset_token.on_cancel([&calls] { ++calls; });
        if (!expect(registration.active(), "the registration is active")) {
            return 16;
        }
        registration.reset();
        if (!expect(!registration.active(), "reset clears the handle") ||
            !expect(reset_token.registration_count() == 0, "reset unregisters")) {
            return 17;
        }
        registration.reset();
        reset_source.cancel();
        if (!expect(calls == 0, "an unregistered callback did not run")) {
            return 18;
        }
    }

    // --- #845: a callback that destroys its own registration does not deadlock ---------------
    {
        CancellationSource self_source;
        const auto self_token = self_source.token();
        int calls = 0;
        // The registration is held through shared state, so the callback can reset it while
        // `cancel()` is iterating this very slot.
        auto holder = std::make_shared<tessera::runtime::CancellationRegistration>();
        *holder = self_token.on_cancel([&calls, holder] {
            ++calls;
            holder->reset();
        });
        self_source.cancel();
        if (!expect(calls == 1, "the self-unregistering callback ran once") ||
            !expect(!holder->active(), "it unregistered itself")) {
            return 19;
        }
    }

    // --- #845: destroying a registration waits for a running callback (TSan-covered) ----------
    {
        CancellationSource cross_source;
        const auto cross_token = cross_source.token();
        std::atomic<bool> callback_running{false};
        std::atomic<bool> reset_started{false};
        std::atomic<bool> release{false};
        auto registration = cross_token.on_cancel([&] {
            callback_running.store(true, std::memory_order_release);
            while (!release.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
        std::thread canceller([&] { cross_source.cancel(); });
        while (!callback_running.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // Reset on another thread while the callback runs on the canceller's: it must block until
        // the callback returns, which is what keeps a captured `this` valid. The resetter publishes
        // that it is about to reset, so the release below lands while it is (very likely) waiting.
        std::thread resetter([&] {
            reset_started.store(true, std::memory_order_release);
            registration.reset();
        });
        while (!reset_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        // A bounded handoff: enough yields for the resetter to reach the wait. Correctness does not
        // depend on it landing there (a reset before the wait is still a valid no-op), only the
        // coverage of the blocking path does.
        for (int spin = 0; spin < 1000; ++spin) {
            std::this_thread::yield();
        }
        release.store(true, std::memory_order_release);
        resetter.join();
        canceller.join();
        if (!expect(!registration.active(),
                    "the cross-thread reset completed after the callback")) {
            return 20;
        }
    }

    std::cout << "async: ok\n";
    return 0;
}
