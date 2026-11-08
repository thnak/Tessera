// SPDX-License-Identifier: MIT

#include "tessera/ui/input.hpp"

#include <stdexcept>

namespace tessera::ui {
namespace {

bool same_pointer_move(const InputEvent& previous, const InputEvent& next) {
    return previous.type == InputEventType::PointerMove &&
           next.type == InputEventType::PointerMove && previous.pointer_id == next.pointer_id;
}

} // namespace

InputQueue::InputQueue(std::size_t capacity) {
    if (capacity == 0) {
        throw std::invalid_argument("input queue capacity must be greater than zero");
    }
    capacity_ = capacity;
    buffer_.resize(capacity_ + 1);
}

bool InputQueue::push(const InputEvent& event) {
    const std::size_t tail = tail_.load(std::memory_order_relaxed);
    const std::size_t head = head_.load(std::memory_order_acquire);
    if (tail - head == capacity_) {
        overflowed_.store(true, std::memory_order_relaxed);
        return false;
    }
    buffer_[tail % buffer_.size()] = event;
    tail_.store(tail + 1, std::memory_order_release);
    return true;
}

bool InputQueue::pop(InputEvent& event) {
    const std::size_t head = head_.load(std::memory_order_relaxed);
    if (head == tail_.load(std::memory_order_acquire)) {
        return false;
    }
    event = buffer_[head % buffer_.size()];
    head_.store(head + 1, std::memory_order_release);
    return true;
}

std::size_t InputQueue::size() const {
    const std::size_t head = head_.load(std::memory_order_acquire);
    const std::size_t tail = tail_.load(std::memory_order_acquire);
    return tail - head;
}

bool InputQueue::empty() const {
    return size() == 0;
}

InputSampler::InputSampler(std::size_t max_events) {
    if (max_events == 0) {
        throw std::invalid_argument("input sampler max_events must be greater than zero");
    }
    events_.reserve(max_events);
}

void InputSampler::sample(InputQueue& queue) {
    events_.clear();

    InputEvent event;
    while (queue.pop(event)) {
        if (!events_.empty() && same_pointer_move(events_.back(), event)) {
            InputEvent& previous = events_.back();
            previous.timestamp_s = event.timestamp_s;
            previous.x = event.x;
            previous.y = event.y;
            previous.dx += event.dx;
            previous.dy += event.dy;
            previous.pressure = event.pressure;
            previous.modifiers = event.modifiers;
            previous.is_primary = event.is_primary;
            continue;
        }
        events_.push_back(event);
    }
}

} // namespace tessera::ui
