// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tessera::ui {

enum class InputEventType : std::uint8_t {
    PointerMove,
    PointerDown,
    PointerUp,
    PointerCancel,
    Wheel,
    KeyDown,
    KeyUp,
    Text,
    CompositionStart,
    CompositionUpdate,
    CompositionEnd,
    Focus,
    Blur,
};

enum class PointerButton : std::uint8_t {
    None,
    Left,
    Right,
    Middle,
    Back,
    Forward,
    Other,
};

using KeyCode = std::uint32_t;

enum class Modifier : std::uint8_t {
    None = 0,
    Shift = 1u << 0,
    Control = 1u << 1,
    Alt = 1u << 2,
    Meta = 1u << 3,
    CapsLock = 1u << 4,
    NumLock = 1u << 5,
};

constexpr Modifier operator|(Modifier lhs, Modifier rhs) {
    return static_cast<Modifier>(static_cast<std::uint8_t>(lhs) | static_cast<std::uint8_t>(rhs));
}

constexpr Modifier operator&(Modifier lhs, Modifier rhs) {
    return static_cast<Modifier>(static_cast<std::uint8_t>(lhs) & static_cast<std::uint8_t>(rhs));
}

constexpr bool has_modifier(Modifier value, Modifier flag) {
    return (value & flag) != Modifier::None;
}

struct InputEvent {
    InputEventType type = InputEventType::PointerMove;
    double timestamp_s = 0.0;
    std::uint32_t device_id = 0;
    std::uint32_t pointer_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    float pressure = 0.0f;
    PointerButton button = PointerButton::None;
    KeyCode key = 0;
    Modifier modifiers = Modifier::None;
    std::uint32_t text_codepoint = 0;
    bool repeat = false;
    bool is_primary = false;
};

class InputQueue {
public:
    explicit InputQueue(std::size_t capacity = 256);
    ~InputQueue() = default;

    InputQueue(const InputQueue&) = delete;
    InputQueue& operator=(const InputQueue&) = delete;

    bool push(const InputEvent& event);
    bool pop(InputEvent& event);

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const {
        return capacity_;
    }
    [[nodiscard]] bool empty() const;
    [[nodiscard]] bool overflowed() const {
        return overflowed_.load(std::memory_order_relaxed);
    }
    void clear_overflow() {
        overflowed_.store(false, std::memory_order_relaxed);
    }

private:
    std::vector<InputEvent> buffer_;
    std::size_t capacity_ = 0;
    std::atomic<std::size_t> head_{0};
    std::atomic<std::size_t> tail_{0};
    std::atomic<bool> overflowed_{false};
};

class InputSampler {
public:
    explicit InputSampler(std::size_t max_events = 1024);

    // Drains the whole queue into one tick's batch.
    void sample(InputQueue& queue);
    // Drains at most `max_events`, leaving the rest queued for the next tick. Adjacent moves of
    // one pointer still coalesce; moves split across ticks do not.
    void sample(InputQueue& queue, std::size_t max_events);

    [[nodiscard]] const std::vector<InputEvent>& events() const {
        return events_;
    }

private:
    std::vector<InputEvent> events_;
};

} // namespace tessera::ui
