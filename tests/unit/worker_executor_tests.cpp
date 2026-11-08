// SPDX-License-Identifier: MIT

#include "tessera/runtime/worker_executor.hpp"

#include <atomic>
#include <iostream>
#include <latch>
#include <thread>

namespace {

using tessera::runtime::CancellationSource;
using tessera::runtime::CompletionPriority;
using tessera::runtime::CompletionQueue;
using tessera::runtime::WorkerExecutor;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "worker_executor: " << message << "\n";
    }
    return condition;
}

} // namespace

int main() {
    {
        CompletionQueue completions(64);
        WorkerExecutor executor(completions, {2, 16});
        std::atomic<int> ran{0};
        std::atomic<int> completed{0};
        for (int i = 0; i < 8; ++i) {
            if (!expect(executor.post(CompletionPriority::Visible,
                                      [&ran, &completed] {
                                          ++ran;
                                          ++completed;
                                      }),
                        "post task")) {
                return 1;
            }
        }
        executor.shutdown();
        executor.shutdown();
        const std::size_t drained = completions.run(64);
        if (!expect(drained == 8, "all completions drained") ||
            !expect(ran == 8, "all tasks ran") ||
            !expect(completed == 8, "completion callbacks ran") ||
            !expect(executor.stats().executed == 8, "executor executed stat") ||
            !expect(executor.stats().pending == 0, "executor queue drained")) {
            return 2;
        }
    }

    {
        CompletionQueue completions(8);
        WorkerExecutor executor(completions);
        CancellationSource source;
        const auto token = source.token();
        source.cancel();
        std::atomic<int> ran{0};
        if (!expect(!executor.post([&ran] { ++ran; }, token), "pre-cancelled task rejected") ||
            !expect(executor.stats().cancelled == 1, "cancelled stat")) {
            return 3;
        }
        executor.shutdown();
        if (!expect(ran == 0, "cancelled task did not run")) {
            return 4;
        }
    }

    {
        CompletionQueue completions(8);
        WorkerExecutor executor(completions, {1, 1});
        std::latch started(1);
        std::latch release(1);
        if (!expect(executor.post([&started, &release] {
                started.count_down();
                release.wait();
            }),
                    "post blocking task")) {
            return 5;
        }
        started.wait();
        if (!expect(executor.post([] {}), "queue accepts one waiting task") ||
            !expect(!executor.post([] {}), "queue rejects overflow") ||
            !expect(executor.stats().dropped == 1, "dropped stat")) {
            return 6;
        }
        release.count_down();
        executor.shutdown();
        if (!expect(executor.stats().executed == 2, "drained queued task on shutdown")) {
            return 7;
        }
    }

    {
        CompletionQueue completions(8);
        WorkerExecutor executor(completions);
        std::atomic<int> ran{0};
        if (!expect(executor.post(CompletionPriority::Background, [&ran] { ++ran; }),
                    "post background completion") ||
            !expect(completions.pending(CompletionPriority::Background) == 0,
                    "completion waits for task")) {
            return 8;
        }
        executor.shutdown();
        if (!expect(completions.pending(CompletionPriority::Background) == 1,
                    "background completion posted") ||
            !expect(completions.run(4) == 1, "background completion drained") ||
            !expect(ran == 1, "completion ran")) {
            return 9;
        }
    }

    {
        CompletionQueue completions(4);
        WorkerExecutor executor(completions);
        if (!expect(!executor.post(std::function<void()>{}), "empty task rejected")) {
            return 10;
        }
        executor.shutdown();
    }

    std::cout << "worker_executor: ok\n";
    return 0;
}
