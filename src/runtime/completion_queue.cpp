// SPDX-License-Identifier: MIT
//
// The completion queue (issues #842, #843): a per-priority fire-and-forget lane beside an unbounded
// per-priority continuation lane, plus the one-wake-per-edge hook a parked host needs.

#include "tessera/runtime/async.hpp"

#include <stdexcept>
#include <utility>

namespace tessera::runtime {

CompletionQueue::CompletionQueue(std::size_t capacity, WakeFn wake, void* wake_context)
    : capacity_(capacity),
      wake_(wake),
      wake_context_(wake_context) {
    if (capacity_ == 0) {
        throw std::invalid_argument("completion queue capacity must be greater than zero");
    }
}

CompletionQueue::~CompletionQueue() {
    // Pop one node at a time and release its reference outside the lock: destroying a frame can
    // post another resume, which this loop then also drains.
    for (;;) {
        std::shared_ptr<void> released;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            wake_ = nullptr;
            detail::ContinuationNode* node = nullptr;
            for (ContinuationLane& lane : continuations_) {
                if (lane.head != nullptr) {
                    node = lane.head;
                    lane.head = node->next_;
                    if (lane.head == nullptr) {
                        lane.tail = nullptr;
                    }
                    node->next_ = nullptr;
                    node->linked_ = false;
                    break;
                }
            }
            if (node == nullptr) {
                return;
            }
            released = node->take_queued_reference();
        }
    }
}

void CompletionQueue::set_wake(WakeFn wake, void* wake_context) noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    wake_ = wake;
    wake_context_ = wake_context;
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

bool CompletionQueue::is_empty_locked() const noexcept {
    return visible_.empty() && layout_paint_.empty() && background_.empty() &&
           continuations_[0].head == nullptr && continuations_[1].head == nullptr &&
           continuations_[2].head == nullptr;
}

void CompletionQueue::wake_if_newly_non_empty() noexcept {
    // Called with `mutex_` held, by every path that can add work. `was_empty_` tracks the state
    // after the previous operation, so a burst of posts while the loop is parked produces one wake
    // and a post after the queue drained produces another.
    const bool empty_now = is_empty_locked();
    const bool newly_non_empty = was_empty_ && !empty_now;
    was_empty_ = empty_now;
    if (newly_non_empty && wake_ != nullptr) {
        wake_(wake_context_);
    }
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

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (visible_.size() + layout_paint_.size() + background_.size() >= capacity_) {
            ++stats_.dropped;
            return false;
        }

        std::deque<Task>& queue =
            index == 0 ? visible_ : (index == 1 ? layout_paint_ : background_);
        queue.push_back(Task{std::move(task), std::move(token)});
        ++stats_.posted;
        wake_if_newly_non_empty();
    }
    return true;
}

bool CompletionQueue::post_continuation(CompletionPriority priority,
                                        detail::ContinuationNode& node) {
    const std::size_t index = priority_index(priority);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (node.linked_) {
            // Already queued: a second resume would corrupt the intrusive list. Consistent with
            // `Task::start()` refusing a double start, this is a no-op, not an error.
            return false;
        }
        ContinuationLane& lane = continuations_[index];
        node.next_ = nullptr;
        node.linked_ = true;
        if (lane.tail == nullptr) {
            lane.head = &node;
        } else {
            lane.tail->next_ = &node;
        }
        lane.tail = &node;
        ++stats_.posted;
        wake_if_newly_non_empty();
    }
    return true;
}

std::size_t CompletionQueue::run(std::size_t budget) {
    std::size_t executed = 0;
    while (executed < budget) {
        // Task continuations drain before fire-and-forget work at the same priority, so a task is
        // never starved behind a burst of plain completions (issue #843). Priority order is
        // visible, layout, background for both kinds of work.
        detail::ContinuationNode* continuation = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (ContinuationLane& lane : continuations_) {
                if (lane.head != nullptr) {
                    continuation = lane.head;
                    lane.head = continuation->next_;
                    if (lane.head == nullptr) {
                        lane.tail = nullptr;
                    }
                    continuation->next_ = nullptr;
                    continuation->linked_ = false;
                    ++stats_.executed;
                    ++stats_.continuations_executed;
                    was_empty_ = is_empty_locked();
                    break;
                }
            }
        }
        if (continuation != nullptr) {
            ++executed;
            continuation->resume_continuation();
            continue;
        }

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
                    was_empty_ = is_empty_locked();
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
    was_empty_ = is_empty_locked();
    return discarded;
}

std::size_t CompletionQueue::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::size_t continuations = 0;
    for (const ContinuationLane& lane : continuations_) {
        for (detail::ContinuationNode* node = lane.head; node != nullptr; node = node->next_) {
            ++continuations;
        }
    }
    return visible_.size() + layout_paint_.size() + background_.size() + continuations;
}

bool CompletionQueue::empty() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return visible_.empty() && layout_paint_.empty() && background_.empty() &&
           continuations_[0].head == nullptr && continuations_[1].head == nullptr &&
           continuations_[2].head == nullptr;
}

std::size_t CompletionQueue::pending(CompletionPriority priority) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::size_t index = priority_index(priority);
    const std::deque<Task>& plain =
        index == 0 ? visible_ : (index == 1 ? layout_paint_ : background_);
    std::size_t continuations = 0;
    for (const detail::ContinuationNode* node = continuations_[index].head; node != nullptr;
         node = node->next_) {
        ++continuations;
    }
    return plain.size() + continuations;
}

CompletionQueue::Stats CompletionQueue::stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    Stats result = stats_;
    std::size_t continuations = 0;
    for (const ContinuationLane& lane : continuations_) {
        for (detail::ContinuationNode* node = lane.head; node != nullptr; node = node->next_) {
            ++continuations;
        }
    }
    result.pending = visible_.size() + layout_paint_.size() + background_.size() + continuations;
    result.pending_visible = visible_.size();
    result.pending_layout = layout_paint_.size();
    result.pending_background = background_.size();
    result.pending_continuations = continuations;
    return result;
}
} // namespace tessera::runtime
