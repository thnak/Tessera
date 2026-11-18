// SPDX-License-Identifier: MIT

#include "retire_queue.hpp"

#include <utility>

namespace tessera::rendering::rhi {

void RetireQueue::retire(std::uint64_t value, std::function<void()> destroy) {
    if (!destroy) {
        return;
    }
    entries_.push_back(Entry{value, std::move(destroy)});
}

void RetireQueue::reap(const CompletionSource& source) {
    if (entries_.empty()) {
        return;
    }
    const std::uint64_t completed = source.completed_value();

    // Detach the ready entries before running anything: a destroyer may retire another resource,
    // which would otherwise invalidate the iteration over entries_.
    ready_.clear();
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->value <= completed) {
            ready_.push_back(std::move(*it));
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
    for (Entry& entry : ready_) {
        entry.destroy();
    }
    ready_.clear();
}

void RetireQueue::drain() {
    if (entries_.empty()) {
        return;
    }
    // Move the whole queue out first, for the same reentrancy reason as reap().
    ready_.clear();
    ready_.swap(entries_);
    entries_.clear();
    for (Entry& entry : ready_) {
        entry.destroy();
    }
    ready_.clear();
}

} // namespace tessera::rendering::rhi
