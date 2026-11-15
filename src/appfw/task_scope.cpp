// SPDX-License-Identifier: MIT

#include "tessera/appfw/task_scope.hpp"

#include <cmath>
#include <utility>

namespace tessera::appfw {

namespace detail {

TaskScopeState::TaskScopeState(runtime::Clock& clock_source, runtime::CompletionQueue& queue_source,
                               TaskScopeConfig scope_config)
    : clock(&clock_source),
      completions(&queue_source),
      config(scope_config),
      slots(std::make_unique<Slot[]>(scope_config.capacity > 0 ? scope_config.capacity : 1)),
      capacity(scope_config.capacity > 0 ? scope_config.capacity : 1) {}

runtime::TaskContext TaskScopeState::child_context() const {
    return runtime::TaskContext{completions, config.priority, cancel.token()};
}

runtime::TaskContext TaskScopeState::driver_context() const {
    // No token: a cancelled driver would be skipped by `TaskStateBase::resume_from_queue`, and the
    // driver's slot retirement is precisely what has to run when a child is cancelled.
    return runtime::TaskContext{completions, config.priority, {}};
}

bool TaskScopeState::reserve(std::size_t& index, std::uint32_t& generation) {
    if (closing || cancel.cancelled()) {
        ++refused;
        return false;
    }
    for (std::size_t cursor = 0; cursor < capacity; ++cursor) {
        Slot& slot = slots[cursor];
        if (slot.live) {
            continue;
        }
        // Reusing the slot is where its previous (finished) handle is dropped. `retire` only marks
        // the slot free, because it runs inside the driver's own frame and destroying the frame
        // that is executing is undefined behaviour.
        slot.task = runtime::Task<void>{};
        slot.live = true;
        ++slot.generation;
        generation = slot.generation;
        index = cursor;
        ++live;
        ++spawned;
        if (live > peak) {
            peak = live;
        }
        return true;
    }
    ++refused;
    return false;
}

bool TaskScopeState::adopt(std::size_t index, std::uint32_t generation, runtime::Task<void> task) {
    Slot& slot = slots[index];
    if (!slot.live || slot.generation != generation) {
        ++refused;
        return false;
    }
    slot.task = std::move(task);
    if (slot.task.start()) {
        return true;
    }
    // The queue refused the first resume: nothing ran, so the slot is released and the attempt is
    // not counted as live work.
    slot.task = runtime::Task<void>{};
    slot.live = false;
    --live;
    --spawned;
    ++refused;
    return false;
}

void TaskScopeState::retire(std::size_t index, std::uint32_t generation) noexcept {
    Slot& slot = slots[index];
    if (!slot.live || slot.generation != generation) {
        return;
    }
    slot.live = false;
    ++finished;
    if (live > 0) {
        --live;
    }
}

void TaskScopeState::record_failure() noexcept {
    ++failed;
}

void TaskScopeState::record_cancelled_child() noexcept {
    ++cancelled_children;
}

} // namespace detail

TaskScope::TaskScope(runtime::Clock& clock, runtime::CompletionQueue& completions,
                     TaskScopeConfig config, runtime::CancellationToken parent)
    : state_(std::make_shared<detail::TaskScopeState>(clock, completions, config)) {
    if (parent.cancelled()) {
        state_->cancel.cancel();
        return;
    }
    // The registration holds the state weakly, so a parent that outlives this scope neither keeps
    // it alive nor writes through a dangling pointer when it is finally cancelled.
    parent.on_cancel([weak = std::weak_ptr<detail::TaskScopeState>(state_)] {
        if (const std::shared_ptr<detail::TaskScopeState> state = weak.lock()) {
            state->cancel.cancel();
        }
    });
}

TaskScope::~TaskScope() {
    state_->closing = true;
    state_->cancel.cancel();
}

runtime::TaskContext TaskScope::context() const {
    return state_->child_context();
}

void TaskScope::cancel() {
    state_->closing = true;
    state_->cancel.cancel();
}

void TaskScope::close(double deadline_seconds) {
    cancel();
    // A second close after the deadline expired must not revive the scope.
    if (state_->timed_out) {
        return;
    }
    state_->deadline_seconds =
        std::isfinite(deadline_seconds) ? deadline_seconds : state_->clock->now_seconds();
}

TaskScopeCloseState TaskScope::tick() {
    if (!state_->closing) {
        return TaskScopeCloseState::Open;
    }
    if (state_->live == 0) {
        return TaskScopeCloseState::Closed;
    }
    if (state_->timed_out) {
        return TaskScopeCloseState::TimedOut;
    }
    if (!(state_->clock->now_seconds() < state_->deadline_seconds)) {
        // The deadline is advisory: a child that ignores its token keeps its frame, so the scope
        // reports the timeout rather than pretending it drained. Frames are released when they
        // finish, never destroyed under a live callback.
        state_->timed_out = true;
        return TaskScopeCloseState::TimedOut;
    }
    return TaskScopeCloseState::Closing;
}

bool TaskScope::cancelled() const {
    return state_->cancel.cancelled();
}

bool TaskScope::closing() const noexcept {
    return state_->closing;
}

bool TaskScope::drained() const noexcept {
    return state_->live == 0;
}

std::size_t TaskScope::live() const noexcept {
    return state_->live;
}

std::size_t TaskScope::peak() const noexcept {
    return state_->peak;
}

std::size_t TaskScope::spawned() const noexcept {
    return state_->spawned;
}

std::size_t TaskScope::finished() const noexcept {
    return state_->finished;
}

std::size_t TaskScope::failed() const noexcept {
    return state_->failed;
}

std::size_t TaskScope::cancelled_children() const noexcept {
    return state_->cancelled_children;
}

std::size_t TaskScope::refused() const noexcept {
    return state_->refused;
}

std::size_t TaskScope::capacity() const noexcept {
    return state_->capacity;
}

} // namespace tessera::appfw
