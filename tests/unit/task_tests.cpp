// SPDX-License-Identifier: MIT

#include "tessera/runtime/async.hpp"
#include "tessera/runtime/task.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using tessera::runtime::CancellationSource;
using tessera::runtime::CompletionPriority;
using tessera::runtime::CompletionQueue;
using tessera::runtime::Task;
using tessera::runtime::TaskCancelled;
using tessera::runtime::TaskContext;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "task: " << message << "\n";
    }
    return condition;
}

TaskContext context(CompletionQueue& queue,
                    CompletionPriority priority = CompletionPriority::Visible) {
    return TaskContext{&queue, priority, {}};
}

// Drains one completion at a time so tests can observe every scheduling step.
std::size_t pump(CompletionQueue& queue, std::size_t budget = 32) {
    std::size_t executed = 0;
    while (!queue.empty() && executed < budget) {
        executed += queue.run(1);
    }
    return executed;
}

// --- coroutine fixtures ---------------------------------------------------------------------

Task<int> doubled(TaskContext, int value) {
    co_return value * 2;
}

Task<void> marked(TaskContext, bool& flag) {
    flag = true;
    co_return;
}

Task<void> value_root(TaskContext ctx, int& out, bool& ran) {
    out = co_await doubled(ctx, 21);
    ran = true;
}

Task<int> add(TaskContext, int a, int b) {
    co_return a + b;
}

Task<void> chain(TaskContext ctx, int& out) {
    const int first = co_await add(ctx, 1, 2);
    const int second = co_await add(ctx, first, 4);
    out = second;
}

Task<int> failing(TaskContext, bool should_fail) {
    if (should_fail) {
        throw std::runtime_error("boom");
    }
    co_return 0;
}

Task<void> catching(TaskContext ctx, bool& caught, std::string& message) {
    try {
        co_await failing(ctx, true);
    } catch (const std::runtime_error& error) {
        caught = true;
        message = error.what();
    }
}

Task<void> record(TaskContext, std::vector<char>& order, char tag) {
    order.push_back(tag);
    co_return;
}

Task<void> wait_for(TaskContext outer_ctx, TaskContext inner_ctx, std::vector<int>& hits) {
    (void)outer_ctx;
    hits.push_back(1);
    hits.push_back(co_await doubled(inner_ctx, 5));
}

Task<void> cancelling_child(TaskContext, CancellationSource& source) {
    source.cancel();
    co_return;
}

Task<void> cancel_between(TaskContext ctx, CancellationSource& source, std::vector<int>& hits) {
    hits.push_back(1);
    co_await cancelling_child(ctx, source);
    hits.push_back(2);
}

Task<void> catch_cancel(TaskContext outer_ctx, TaskContext child_ctx, bool& caught, bool& ran) {
    (void)outer_ctx;
    try {
        co_await marked(child_ctx, ran);
    } catch (const TaskCancelled&) {
        caught = true;
    }
}

Task<void> counted(TaskContext, int& runs) {
    ++runs;
    co_return;
}

Task<void> awaited_twice(TaskContext ctx, bool& threw) {
    auto inner = doubled(ctx, 1);
    const int first = co_await inner;
    (void)first;
    try {
        (void)co_await inner;
    } catch (const std::logic_error&) {
        threw = true;
    }
}

Task<void> doomed(TaskContext ctx) {
    co_await failing(ctx, true);
}

} // namespace

int main() {
    {
        CompletionQueue queue(8);
        int result = 0;
        bool ran = false;
        auto task = value_root(context(queue), result, ran);
        if (!expect(!task.started(), "task is lazy before start") ||
            !expect(queue.empty(), "nothing posted before start") ||
            !expect(task.start(), "start schedules the first resume") ||
            !expect(queue.size() == 1, "one resume posted") || !expect(!ran, "body did not run") ||
            !expect(pump(queue) == 3, "root, child, and continuation resumes") ||
            !expect(result == 42, "co_await returned the value") ||
            !expect(ran, "root body completed") || !expect(task.done(), "task done") ||
            !expect(!task.cancelled(), "task not cancelled")) {
            return 1;
        }
    }

    {
        CompletionQueue queue(8);
        bool flag = false;
        auto task = marked(context(queue), flag);
        if (!expect(task.start(), "void start") || !expect(pump(queue) == 1, "void resume") ||
            !expect(flag, "void body ran") || !expect(task.done(), "void done")) {
            return 2;
        }
    }

    {
        CompletionQueue queue(8);
        int out = 0;
        auto task = chain(context(queue), out);
        if (!expect(task.start(), "chain start") ||
            !expect(queue.run(1) == 1 && out == 0, "outer runs to the first await") ||
            !expect(queue.run(1) == 1 && out == 0, "first child completes") ||
            !expect(queue.run(1) == 1 && out == 0, "outer runs to the second await") ||
            !expect(queue.run(1) == 1 && out == 0, "second child completes") ||
            !expect(queue.run(1) == 1 && out == 7, "chained value stored") ||
            !expect(queue.empty() && task.done(), "chain finished")) {
            return 3;
        }
    }

    {
        CompletionQueue queue(8);
        bool caught = false;
        std::string message;
        auto task = catching(context(queue), caught, message);
        if (!expect(task.start(), "exception start") ||
            !expect(pump(queue) == 3, "exception resumes") || !expect(caught, "exception caught") ||
            !expect(message == "boom", "exception message") ||
            !expect(task.done() && !task.cancelled(), "catcher completed")) {
            return 4;
        }
    }

    {
        CompletionQueue queue(8);
        auto task = doomed(context(queue));
        if (!expect(task.start(), "throwing root start") ||
            !expect(pump(queue) == 3, "throwing root resumes") ||
            !expect(task.done(), "throwing root done") ||
            !expect(task.exception() != nullptr, "throwing root stores the error")) {
            return 5;
        }
    }

    {
        CompletionQueue queue(8);
        CancellationSource source;
        source.cancel();
        bool ran = false;
        auto task = marked(TaskContext{&queue, CompletionPriority::Visible, source.token()}, ran);
        if (!expect(task.start(), "cancelled start still posts") ||
            !expect(queue.size() == 1, "cancelled resume posted") ||
            !expect(pump(queue) == 1, "cancelled resume processed") ||
            !expect(!ran, "cancelled body did not run") ||
            !expect(task.done() && task.cancelled(), "cancelled task finished") ||
            !expect(task.exception() != nullptr, "cancellation error stored") ||
            !expect(queue.empty(), "cancelled queue drained")) {
            return 6;
        }
    }

    {
        CompletionQueue queue(8);
        CancellationSource source;
        bool ran = false;
        auto task = marked(TaskContext{&queue, CompletionPriority::Visible, source.token()}, ran);
        if (!expect(task.start(), "late cancel start") ||
            !expect(queue.size() == 1, "resume queued")) {
            return 7;
        }
        source.cancel();
        if (!expect(pump(queue) == 1, "resume checks cancellation") ||
            !expect(!ran, "late cancellation skips the body") ||
            !expect(task.cancelled(), "late cancellation reported")) {
            return 7;
        }
    }

    {
        CompletionQueue queue(8);
        CancellationSource source;
        source.cancel();
        bool caught = false;
        bool ran = false;
        TaskContext child{&queue, CompletionPriority::Visible, source.token()};
        auto task = catch_cancel(context(queue), child, caught, ran);
        if (!expect(task.start(), "awaiter start") ||
            !expect(pump(queue) == 3, "awaiter resumes") ||
            !expect(caught, "TaskCancelled propagates at the await boundary") ||
            !expect(!ran, "cancelled child body did not run")) {
            return 8;
        }
    }

    {
        CompletionQueue queue(8);
        CancellationSource source;
        std::vector<int> hits;
        TaskContext ctx{&queue, CompletionPriority::Visible, source.token()};
        auto task = cancel_between(ctx, source, hits);
        if (!expect(task.start(), "cancel-between start") ||
            !expect(pump(queue) == 3, "cancel-between resumes") ||
            !expect(hits == std::vector<int>{1}, "no body after the cancelled await") ||
            !expect(task.done() && task.cancelled(), "cancel-between task cancelled")) {
            return 9;
        }
    }

    {
        CompletionQueue queue(8);
        std::vector<char> order;
        auto background = record(context(queue, CompletionPriority::Background), order, 'b');
        auto visible = record(context(queue, CompletionPriority::Visible), order, 'v');
        if (!expect(background.start(), "background start") ||
            !expect(visible.start(), "visible start") ||
            !expect(queue.pending(CompletionPriority::Visible) == 1, "visible pending") ||
            !expect(queue.pending(CompletionPriority::Background) == 1, "background pending") ||
            !expect(queue.run(1) == 1 && order == std::vector<char>{'v'}, "visible runs first") ||
            !expect(queue.run(1) == 1 && order == std::vector<char>{'v', 'b'},
                    "background runs last") ||
            !expect(background.done() && visible.done(), "priority tasks done")) {
            return 10;
        }
    }

    {
        CompletionQueue queue(8);
        std::vector<int> hits;
        TaskContext outer{&queue, CompletionPriority::Visible, {}};
        TaskContext inner{&queue, CompletionPriority::Background, {}};
        auto task = wait_for(outer, inner, hits);
        if (!expect(task.start(), "outer start") ||
            !expect(queue.pending(CompletionPriority::Visible) == 1, "outer resume queued") ||
            !expect(queue.run(1) == 1 && hits == std::vector<int>{1}, "outer runs to the await") ||
            !expect(queue.pending(CompletionPriority::Background) == 1,
                    "inner resume at its priority") ||
            !expect(queue.run(1) == 1 && hits == std::vector<int>{1}, "inner completes") ||
            !expect(queue.pending(CompletionPriority::Visible) == 1,
                    "continuation at the awaiter priority") ||
            !expect(queue.run(1) == 1 && hits == std::vector<int>{1, 10},
                    "continuation resumes on the queue")) {
            return 11;
        }
    }

    {
        CompletionQueue queue(8);
        int runs = 0;
        auto task = counted(context(queue), runs);
        if (!expect(task.start(), "counted start") ||
            !expect(!task.start(), "second start rejected") ||
            !expect(pump(queue) == 1, "one resume") || !expect(runs == 1, "body ran once") ||
            !expect(queue.stats().executed == 1, "one completion executed") ||
            !expect(!task.start(), "start after completion rejected") ||
            !expect(pump(queue) == 0, "nothing left to resume") ||
            !expect(runs == 1, "still one run")) {
            return 12;
        }
    }

    {
        CompletionQueue queue(8);
        CancellationSource source;
        int runs = 0;
        auto task = counted(TaskContext{&queue, CompletionPriority::Visible, source.token()}, runs);
        if (!expect(task.start(), "cancel-after-done start") ||
            !expect(pump(queue) == 1, "completed") ||
            !expect(task.done() && !task.cancelled(), "completed not cancelled")) {
            return 13;
        }
        source.cancel();
        if (!expect(task.done() && !task.cancelled(),
                    "cancellation does not rewrite a finished task") ||
            !expect(pump(queue) == 0, "no resume after completion") ||
            !expect(runs == 1, "no re-run")) {
            return 13;
        }
    }

    {
        CompletionQueue queue(8);
        bool threw = false;
        auto task = awaited_twice(context(queue), threw);
        if (!expect(task.start(), "double-await start") ||
            !expect(pump(queue) == 3, "double-await resumes") ||
            !expect(threw, "second await rejected")) {
            return 14;
        }
    }

    std::cout << "task: ok\n";
    return 0;
}
