// SPDX-License-Identifier: MIT

#include "tessera/runtime/async.hpp"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>
#include <vector>

namespace tessera::runtime {

struct detail::CancellationState {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::vector<std::function<void()>> callbacks;
};

namespace {

void register_cancel_callback(const std::shared_ptr<detail::CancellationState>& state,
                              std::function<void()> callback) {
    if (state == nullptr || !callback) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (!state->cancelled.load(std::memory_order_acquire)) {
            state->callbacks.push_back(std::move(callback));
            return;
        }
    }
    callback();
}

} // namespace

CancellationToken CancellationSource::token() const {
    return CancellationToken{state_};
}

bool CancellationToken::cancelled() const {
    return state_ != nullptr && state_->cancelled.load(std::memory_order_acquire);
}

void CancellationToken::on_cancel(std::function<void()> callback) const {
    register_cancel_callback(state_, std::move(callback));
}

CancellationSource::CancellationSource()
    : state_(std::make_shared<detail::CancellationState>()) {}

void CancellationSource::cancel() {
    if (state_->cancelled.exchange(true, std::memory_order_acq_rel)) {
        return;
    }

    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        callbacks.swap(state_->callbacks);
    }
    for (std::function<void()>& callback : callbacks) {
        if (callback) {
            callback();
        }
    }
}

bool CancellationSource::cancelled() const {
    return state_->cancelled.load(std::memory_order_acquire);
}

void CancellationSource::on_cancel(std::function<void()> callback) const {
    register_cancel_callback(state_, std::move(callback));
}

CompletionQueue::CompletionQueue(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("completion queue capacity must be greater than zero");
    }
}

std::size_t CompletionQueue::priority_index(CompletionPriority priority) {
    switch (priority) {
    case CompletionPriority::Visible:
        return 0;
    case CompletionPriority::LayoutPaint:
        return 1;
    case CompletionPriority::Background:
        return 2;
    }
    throw std::invalid_argument("invalid completion priority");
}

bool CompletionQueue::post(CompletionPriority priority, std::function<void()> task,
                           CancellationToken token) {
    const std::size_t index = priority_index(priority);
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
    if (visible_.size() + layout_paint_.size() + background_.size() >= capacity_) {
        ++stats_.dropped;
        return false;
    }

    std::deque<Task>& queue = index == 0 ? visible_ : (index == 1 ? layout_paint_ : background_);
    queue.push_back(Task{std::move(task), std::move(token)});
    ++stats_.posted;
    return true;
}

std::size_t CompletionQueue::run(std::size_t budget) {
    std::size_t executed = 0;
    while (executed < budget) {
        Task task;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            std::deque<Task>* queues[] = {&visible_, &layout_paint_, &background_};
            for (std::deque<Task>* queue : queues) {
                if (!queue->empty()) {
                    task = std::move(queue->front());
                    queue->pop_front();
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            break;
        }
        if (task.token.cancelled()) {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.cancelled;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++stats_.executed;
        }
        ++executed;
        task.function();
    }
    return executed;
}

std::size_t CompletionQueue::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t discarded = visible_.size() + layout_paint_.size() + background_.size();
    visible_.clear();
    layout_paint_.clear();
    background_.clear();
    stats_.discarded += discarded;
    return discarded;
}

std::size_t CompletionQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return visible_.size() + layout_paint_.size() + background_.size();
}

bool CompletionQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return visible_.empty() && layout_paint_.empty() && background_.empty();
}

std::size_t CompletionQueue::pending(CompletionPriority priority) const {
    std::lock_guard<std::mutex> lock(mutex_);
    switch (priority) {
    case CompletionPriority::Visible:
        return visible_.size();
    case CompletionPriority::LayoutPaint:
        return layout_paint_.size();
    case CompletionPriority::Background:
        return background_.size();
    }
    return 0;
}

CompletionQueue::Stats CompletionQueue::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats result = stats_;
    result.pending = visible_.size() + layout_paint_.size() + background_.size();
    result.pending_visible = visible_.size();
    result.pending_layout = layout_paint_.size();
    result.pending_background = background_.size();
    return result;
}

ManualExecutor::ManualExecutor(std::size_t capacity)
    : capacity_(capacity) {
    if (capacity_ == 0) {
        throw std::invalid_argument("manual executor capacity must be greater than zero");
    }
}

bool ManualExecutor::post(std::function<void()> task, CancellationToken token) {
    if (!task) {
        ++stats_.dropped;
        return false;
    }
    if (token.cancelled()) {
        ++stats_.cancelled;
        return false;
    }
    if (tasks_.size() >= capacity_) {
        ++stats_.dropped;
        return false;
    }

    tasks_.push_back(Task{std::move(task), std::move(token)});
    ++stats_.posted;
    return true;
}

bool ManualExecutor::run_one() {
    while (!tasks_.empty()) {
        Task task = std::move(tasks_.front());
        tasks_.pop_front();

        if (task.token.cancelled()) {
            ++stats_.cancelled;
            continue;
        }

        ++stats_.executed;
        task.function();
        return true;
    }
    return false;
}

std::size_t ManualExecutor::run(std::size_t budget) {
    std::size_t executed = 0;
    while (executed < budget && run_one()) {
        ++executed;
    }
    return executed;
}

std::size_t ManualExecutor::clear() {
    const std::size_t discarded = tasks_.size();
    stats_.discarded += discarded;
    tasks_.clear();
    return discarded;
}

ManualExecutor::Stats ManualExecutor::stats() const {
    Stats result = stats_;
    result.pending = tasks_.size();
    return result;
}

} // namespace tessera::runtime
