// SPDX-License-Identifier: MIT

#include "tessera/runtime/worker_executor.hpp"

#include <algorithm>
#include <utility>

namespace tessera::runtime {

WorkerExecutor::WorkerExecutor(CompletionQueue& completions)
    : WorkerExecutor(completions, Config{}) {}

WorkerExecutor::WorkerExecutor(CompletionQueue& completions, Config config)
    : completions_(&completions),
      config_(config) {
    config_.workers = std::max<std::size_t>(1, config_.workers);
    config_.capacity = std::max<std::size_t>(1, config_.capacity);
    try {
        workers_.reserve(config_.workers);
        for (std::size_t i = 0; i < config_.workers; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    } catch (...) {
        shutdown();
        throw;
    }
}

WorkerExecutor::~WorkerExecutor() {
    shutdown();
}

bool WorkerExecutor::post(std::function<void()> task, CancellationToken token) {
    if (!task) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.dropped;
        return false;
    }
    if (token.cancelled()) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.cancelled;
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (stopping_ || tasks_.size() >= config_.capacity) {
        ++stats_.dropped;
        return false;
    }
    tasks_.push_back(Task{std::move(task), std::move(token)});
    ++stats_.posted;
    wake_.notify_one();
    return true;
}

bool WorkerExecutor::post(CompletionPriority priority, std::function<void()> completion,
                          CancellationToken token) {
    return post([this, priority, completion = std::move(completion),
                 token]() mutable { completions_->post(priority, std::move(completion), token); },
                token);
}

void WorkerExecutor::shutdown() {
    std::lock_guard<std::mutex> shutdown_lock(shutdown_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
    }
    wake_.notify_all();
    for (std::thread& worker : workers_) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers_.clear();
}

WorkerExecutor::Stats WorkerExecutor::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats result = stats_;
    result.pending = tasks_.size();
    return result;
}

std::size_t WorkerExecutor::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return tasks_.size();
}

void WorkerExecutor::worker_loop() {
    for (;;) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
            if (tasks_.empty()) {
                // stopping_ is only set by shutdown(); an empty queue then means drain complete.
                return;
            }
            task = std::move(tasks_.front());
            tasks_.pop_front();
            ++stats_.active;
        }

        if (task.token.cancelled()) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.cancelled;
            --stats_.active;
            continue;
        }

        task.function();

        std::lock_guard<std::mutex> lock(mutex_);
        ++stats_.executed;
        --stats_.active;
    }
}

} // namespace tessera::runtime
