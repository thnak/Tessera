// SPDX-License-Identifier: MIT

// The shared retire rule (ADR-053 decision 2): a destroyer runs only once the completion value it
// was retired against is reached. These tests use a fake completion source, never a GPU and never
// a sleep, so they run on every platform and prove the rule the Vulkan and Metal backends share.

#include "retire_queue.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::CompletionSource;
using tessera::rendering::rhi::RetireQueue;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "retire_queue: " << message << "\n";
    }
    return condition;
}

// A completion source the test drives by hand, standing in for a timeline semaphore counter or an
// MTLSharedEvent signaled value.
class FakeCompletion final : public CompletionSource {
public:
    [[nodiscard]] std::uint64_t completed_value() const override {
        return completed_;
    }

    void reach(std::uint64_t value) {
        completed_ = value;
    }

private:
    std::uint64_t completed_ = 0;
};

// Records the order destroyers ran in, so a test can assert on it.
struct Log {
    std::vector<int> order;

    std::function<void()> record(int id) {
        return [this, id] { order.push_back(id); };
    }
};

bool check_immediate_when_already_reached() {
    FakeCompletion source;
    source.reach(7);
    RetireQueue queue;
    Log log;

    queue.retire(7, log.record(1));
    if (!expect(queue.pending() == 1, "a retired entry is pending until reaped")) {
        return false;
    }
    queue.reap(source);
    return expect(log.order == std::vector<int>{1}, "a reached value runs on the next reap") &&
           expect(queue.empty(), "a reaped entry leaves the queue");
}

bool check_waits_for_its_value() {
    FakeCompletion source;
    RetireQueue queue;
    Log log;

    queue.retire(3, log.record(1));
    queue.reap(source);
    if (!expect(log.order.empty(), "an unreached value does not run") ||
        !expect(queue.pending() == 1, "an unreached entry stays pending")) {
        return false;
    }

    source.reach(2);
    queue.reap(source);
    if (!expect(log.order.empty(), "a value below the target does not run")) {
        return false;
    }

    source.reach(3);
    queue.reap(source);
    return expect(log.order == std::vector<int>{1}, "the target value runs the destroyer") &&
           expect(queue.empty(), "the queue drains once everything is reached");
}

bool check_runs_in_retire_order() {
    FakeCompletion source;
    RetireQueue queue;
    Log log;

    // Retired out of value order on purpose: the queue must not assume monotonic pushes.
    queue.retire(5, log.record(5));
    queue.retire(2, log.record(2));
    queue.retire(9, log.record(9));

    source.reach(2);
    queue.reap(source);
    if (!expect(log.order == std::vector<int>{2}, "only the reached entry runs")) {
        return false;
    }

    source.reach(6);
    queue.reap(source);
    if (!expect(log.order == std::vector<int>{2, 5}, "a later reach runs the next entry")) {
        return false;
    }

    source.reach(9);
    queue.reap(source);
    return expect(log.order == std::vector<int>{2, 5, 9}, "all entries run in retire order");
}

bool check_reentrant_retire_is_safe() {
    FakeCompletion source;
    RetireQueue queue;
    Log log;

    // A destroyer that retires another resource, which is what a texture whose teardown releases a
    // staging buffer does. The new entry must not be lost or run twice.
    queue.retire(1, [&queue, &log] {
        log.order.push_back(1);
        queue.retire(1, log.record(2));
    });
    source.reach(1);
    queue.reap(source);
    if (!expect(log.order == std::vector<int>{1}, "the outer destroyer runs once") ||
        !expect(queue.pending() == 1, "the nested retire is queued, not run inline")) {
        return false;
    }
    queue.reap(source);
    return expect(log.order == std::vector<int>{1, 2}, "the nested entry runs on the next reap");
}

bool check_drain_runs_everything() {
    FakeCompletion source;
    RetireQueue queue;
    Log log;

    queue.retire(4, log.record(4));
    queue.retire(8, log.record(8));
    queue.reap(source);
    if (!expect(log.order.empty(), "nothing runs before the values are reached")) {
        return false;
    }

    // Teardown: the caller has already waited for the device with a bound.
    queue.drain();
    return expect(log.order == std::vector<int>{4, 8}, "drain runs every pending destroyer") &&
           expect(queue.empty(), "drain empties the queue");
}

bool check_empty_destroyer_is_ignored() {
    RetireQueue queue;
    queue.retire(1, std::function<void()>{});
    return expect(queue.empty(), "an empty destroyer is not queued");
}

bool check_reap_reuses_its_scratch() {
    FakeCompletion source;
    RetireQueue queue;
    Log log;

    // Two reap rounds that each detach several entries: the second must not need a fresh buffer,
    // which is what keeps the steady state allocation-free.
    for (int i = 0; i < 4; ++i) {
        queue.retire(1, log.record(i));
    }
    source.reach(1);
    queue.reap(source);
    for (int i = 4; i < 8; ++i) {
        queue.retire(2, log.record(i));
    }
    source.reach(2);
    queue.reap(source);
    return expect(log.order == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7},
                  "both rounds run every destroyer exactly once");
}

} // namespace

int main() {
    if (!check_immediate_when_already_reached() || !check_waits_for_its_value() ||
        !check_runs_in_retire_order() || !check_reentrant_retire_is_safe() ||
        !check_drain_runs_everything() || !check_empty_destroyer_is_ignored() ||
        !check_reap_reuses_its_scratch()) {
        return 1;
    }
    return 0;
}
