// SPDX-License-Identifier: MIT

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/runtime_loop.hpp"

#include <iostream>

namespace {

using tessera::runtime::CompletionPriority;
using tessera::runtime::CompletionQueue;
using tessera::runtime::FixedStepConfig;
using tessera::runtime::ManualClock;
using tessera::runtime::RuntimeLoop;
using tessera::runtime::RuntimeLoopConfig;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "runtime_loop_async: " << message << "\n";
    }
    return condition;
}

RuntimeLoopConfig config(CompletionQueue* queue, std::size_t budget) {
    RuntimeLoopConfig result;
    result.fixed_step = FixedStepConfig{60.0, 250.0, 5, 1.0};
    result.render_on_demand = true;
    result.completion_queue = queue;
    result.completion_budget = budget;
    return result;
}

} // namespace

int main() {
    ManualClock clock;
    CompletionQueue queue(8);
    RuntimeLoop loop(clock, config(&queue, 2));
    int runs = 0;
    queue.post(CompletionPriority::Visible, [&] {
        ++runs;
        loop.request_frame();
    });
    const auto requested = loop.tick();
    if (!expect(requested.completions == 1, "completion drained") ||
        !expect(runs == 1, "callback ran") ||
        !expect(requested.should_render, "callback frame request renders") ||
        !expect(!requested.completions_pending, "no pending completions")) {
        return 1;
    }

    ManualClock pending_clock;
    CompletionQueue pending_queue(8);
    RuntimeLoop pending_loop(pending_clock, config(&pending_queue, 1));
    int pending_runs = 0;
    pending_queue.post(CompletionPriority::Visible, [&] { ++pending_runs; });
    pending_queue.post(CompletionPriority::Visible, [&] { ++pending_runs; });
    const auto first = pending_loop.tick();
    if (!expect(first.completions == 1, "budget drains one") ||
        !expect(first.completions_pending, "pending reported") ||
        !expect(!first.should_render, "pending does not render before callback")) {
        return 2;
    }
    const auto second = pending_loop.tick();
    if (!expect(second.completions == 1, "second completion drained") ||
        !expect(second.should_render, "pending wakes next tick") ||
        !expect(!second.completions_pending, "pending cleared") ||
        !expect(pending_runs == 2, "both callbacks ran")) {
        return 3;
    }

    ManualClock dirty_clock;
    CompletionQueue dirty_queue(8);
    RuntimeLoop dirty_loop(dirty_clock, config(&dirty_queue, 4));
    dirty_queue.post(CompletionPriority::LayoutPaint, [&] { dirty_loop.mark_render_dirty(); });
    const auto dirty = dirty_loop.tick();
    if (!expect(dirty.completions == 1, "dirty completion drained") ||
        !expect(dirty.should_render, "dirty completion renders")) {
        return 4;
    }

    ManualClock plain_clock;
    RuntimeLoop plain_loop(plain_clock,
                           RuntimeLoopConfig{FixedStepConfig{60.0, 250.0, 5, 1.0}, true});
    const auto plain = plain_loop.tick();
    if (!expect(plain.completions == 0, "no queue completions") ||
        !expect(!plain.completions_pending, "no queue pending")) {
        return 5;
    }

    ManualClock setter_clock;
    CompletionQueue setter_queue(8);
    RuntimeLoop setter_loop(setter_clock,
                            RuntimeLoopConfig{FixedStepConfig{60.0, 250.0, 5, 1.0}, true});
    setter_loop.set_completion_queue(&setter_queue);
    setter_loop.set_completion_budget(0);
    int setter_runs = 0;
    setter_queue.post(CompletionPriority::Visible, [&] { ++setter_runs; });
    const auto setter = setter_loop.tick();
    if (!expect(setter.completions == 0, "zero budget executes nothing") ||
        !expect(setter_runs == 0, "zero budget callback not run") ||
        !expect(setter.completions_pending, "zero budget keeps pending")) {
        return 6;
    }

    std::cout << "runtime_loop_async: ok\n";
    return 0;
}
