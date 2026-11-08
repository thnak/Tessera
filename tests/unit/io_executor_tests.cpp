// SPDX-License-Identifier: MIT

#include "tessera/runtime/io_executor.hpp"
#include "tessera/runtime/worker_executor.hpp"

#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <utility>

namespace {

using tessera::runtime::CancellationSource;
using tessera::runtime::CompletionPriority;
using tessera::runtime::CompletionQueue;
using tessera::runtime::IoExecutor;
using tessera::runtime::WorkerExecutor;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "io_executor: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    // A single in-flight slot rejects a second job while the first is blocked, and the slot is
    // released before the completion is posted.
    {
        CompletionQueue queue(64);
        WorkerExecutor workers(queue, WorkerExecutor::Config{1, 64});
        IoExecutor io(workers, queue, IoExecutor::Config{1});

        std::promise<void> started;
        std::promise<void> release;
        auto started_future = started.get_future();
        auto release_future = release.get_future();
        std::atomic<int> job_calls{0};
        std::atomic<int> completions{0};

        const bool accepted = io.submit(
            [&] {
                ++job_calls;
                started.set_value();
                release_future.wait();
            },
            [&] { ++completions; });
        if (!expect(accepted, "first job accepted") ||
            !expect(started_future.wait_for(std::chrono::seconds(2)) == std::future_status::ready,
                    "job started") ||
            !expect(io.stats().in_flight == 1, "one job in flight")) {
            return 1;
        }

        if (!expect(!io.submit([] {}), "second job rejected at the in-flight limit") ||
            !expect(io.stats().rejected == 1, "rejection counted")) {
            return 2;
        }

        release.set_value();
        workers.shutdown();
        if (!expect(io.stats().in_flight == 0, "slot released") ||
            !expect(io.stats().completed == 1, "job completed") ||
            !expect(job_calls == 1, "job body ran once")) {
            return 3;
        }
        queue.run(64);
        if (!expect(completions == 1, "completion ran on the queue")) {
            return 4;
        }
    }

    // A token already cancelled before submission is rejected without consuming a slot.
    {
        CompletionQueue queue(64);
        WorkerExecutor workers(queue, WorkerExecutor::Config{1, 64});
        IoExecutor io(workers, queue, IoExecutor::Config{1});
        CancellationSource source;
        source.cancel();
        std::atomic<int> job_calls{0};
        if (!expect(
                !io.submit([&] { ++job_calls; }, {}, CompletionPriority::Visible, source.token()),
                "cancelled token rejected") ||
            !expect(io.stats().cancelled == 1, "up-front cancellation counted") ||
            !expect(job_calls == 0, "cancelled job never ran")) {
            return 5;
        }
        workers.shutdown();
    }

    // Cancellation while queued must still release the slot; the wrapper skips the job body.
    {
        CompletionQueue queue(64);
        WorkerExecutor workers(queue, WorkerExecutor::Config{1, 64});
        IoExecutor io(workers, queue, IoExecutor::Config{2});
        std::promise<void> started;
        std::promise<void> release;
        auto started_future = started.get_future();
        auto release_future = release.get_future();

        const bool first = io.submit([&] {
            started.set_value();
            release_future.wait();
        });
        started_future.wait();
        CancellationSource source;
        std::atomic<int> second_calls{0};
        const bool second =
            io.submit([&] { ++second_calls; }, {}, CompletionPriority::Visible, source.token());
        source.cancel();
        release.set_value();
        workers.shutdown();

        if (!expect(first && second, "both jobs accepted") ||
            !expect(second_calls == 0, "queued cancelled job skipped") ||
            !expect(io.stats().in_flight == 0, "cancelled slot released") ||
            !expect(io.stats().cancelled == 1, "queued cancellation counted")) {
            return 6;
        }
    }

    // Completion priority is preserved and a throwing job still releases its slot.
    {
        CompletionQueue queue(64);
        WorkerExecutor workers(queue, WorkerExecutor::Config{1, 64});
        IoExecutor io(workers, queue, IoExecutor::Config{2});
        std::atomic<int> completions{0};
        if (!expect(io.submit([] {}, [&] { ++completions; }, CompletionPriority::LayoutPaint),
                    "priority job accepted") ||
            !expect(io.submit([] { throw 1; }), "throwing job accepted")) {
            return 7;
        }
        workers.shutdown();
        if (!expect(io.stats().in_flight == 0, "throwing job released its slot") ||
            !expect(io.stats().failed == 1, "throwing job counted as failed") ||
            !expect(queue.pending(CompletionPriority::LayoutPaint) == 1,
                    "completion keeps its priority")) {
            return 8;
        }
        queue.run(64);
        if (!expect(completions == 1, "priority completion ran")) {
            return 9;
        }
    }

    std::cout << "io_executor: ok\n";
    return 0;
}
