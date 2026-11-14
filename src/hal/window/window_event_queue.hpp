// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/hal/window.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <span>

namespace tessera::hal::detail {

// Small fixed-capacity ring shared by the platform windows. OS callbacks only record, so an
// overflow drops the oldest event instead of allocating; repeated resizes (and repeated safe-area
// changes) coalesce into the newest metrics. This header is private to src/platform
// implementations.
class EventQueue {
public:
    static constexpr std::size_t kCapacity = 64;

    void push(const WindowEvent& event) {
        if ((event.type == WindowEventType::Resize ||
             event.type == WindowEventType::SafeAreaChanged) &&
            count_ > 0) {
            WindowEvent& newest = at(count_ - 1);
            if (newest.type == event.type) {
                newest = event;
                return;
            }
        }
        if (count_ == events_.size()) {
            head_ = (head_ + 1) % events_.size();
            --count_;
        }
        events_[(head_ + count_) % events_.size()] = event;
        ++count_;
    }

    std::size_t copy_to(std::span<WindowEvent> events) {
        const std::size_t copied = std::min(events.size(), count_);
        for (std::size_t i = 0; i < copied; ++i) {
            events[i] = at(i);
        }
        head_ = (head_ + copied) % events_.size();
        count_ -= copied;
        return copied;
    }

    [[nodiscard]] std::size_t count() const {
        return count_;
    }

    void clear() {
        head_ = 0;
        count_ = 0;
    }

private:
    WindowEvent& at(std::size_t index) {
        return events_[(head_ + index) % events_.size()];
    }

    std::array<WindowEvent, kCapacity> events_{};
    std::size_t head_ = 0;
    std::size_t count_ = 0;
};

} // namespace tessera::hal::detail
