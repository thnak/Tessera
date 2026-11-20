// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/detail/cache_line.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace tessera::ui {

// Maximum UTF-8 bytes carried inline by one composition/edit event. The payload is truncated at a
// UTF-8 sequence boundary and `text_truncated` is set; events never allocate.
inline constexpr std::size_t kMaxEditTextBytes = 256;

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
    // Multi-codepoint committed text (IME result string or platform text input). Carries the
    // bounded inline payload below; single-codepoint keyboard text stays `Text`.
    EditText,
    // System buttons (docs/specs/studio-device-profiles-v1.md section 4, os-events-v1.md section
    // 7): the platform's Back request (Android button or gesture, desktop Escape/browser back) and
    // a hardware volume key. Discrete, never coalesced, and delivered in queue order with the rest
    // of input (os-events-v1.md section 3). Back carries no payload; VolumeKey carries
    // `volume_direction`.
    BackRequested,
    VolumeKey,
};

enum class VolumeDirection : std::uint8_t {
    Up,
    Down,
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

// Physical device kind of a pointer event. Mouse and pen can hover; touch cannot. The default is
// `Mouse`, so normalized producers that do not know the device keep the pre-kind behavior.
enum class PointerKind : std::uint8_t {
    Mouse,
    Touch,
    Pen,
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

// One normalized input event. Members are grouped by alignment so the struct carries no internal
// padding (security review finding C5): declared in the order the fields were added, it had 24
// bytes of holes in 344 - seven after `type` to align the `double`, three after `pointer_kind`,
// and four of tail padding - and the default 256-event queue copies it on every push and pop.
// Grouping takes it to 320, which is 6 KiB off the queue. `InputEvent` is only ever built as
// `InputEvent{}` and assigned field by field, so the order carries no meaning and the reorder is
// behavior-preserving.
struct InputEvent {
    // 8-byte group.
    double timestamp_s = 0.0;
    // 4-byte group.
    std::uint32_t device_id = 0;
    std::uint32_t pointer_id = 0;
    float x = 0.0f;
    float y = 0.0f;
    float dx = 0.0f;
    float dy = 0.0f;
    float pressure = 0.0f;
    KeyCode key = 0;
    std::uint32_t text_codepoint = 0;
    // Bounded inline composition/edit payload: UTF-8 bytes. `text_size` counts valid bytes and
    // `text_truncated` is set when the platform string did not fit `kMaxEditTextBytes`. The
    // selection range is UTF-8 byte offsets into `text` (the HAL converts the platform's UTF-16
    // IME caret), so it uses the same unit as the payload and as `TextChange`.
    std::uint32_t text_size = 0;
    std::uint32_t selection_start = 0;
    std::uint32_t selection_end = 0;
    // The payload itself, byte-aligned, so it starts on a 4-byte boundary and needs no padding.
    std::array<char, kMaxEditTextBytes> text{};
    // 1-byte group.
    InputEventType type = InputEventType::PointerMove;
    PointerKind pointer_kind = PointerKind::Mouse;
    PointerButton button = PointerButton::None;
    Modifier modifiers = Modifier::None;
    bool repeat = false;
    bool is_primary = false;
    bool text_truncated = false;
    // Meaningful for InputEventType::VolumeKey only.
    VolumeDirection volume_direction = VolumeDirection::Up;
};

// Memory budget of one queued input event (docs/design/performance.md, memory budgets). The
// default `InputQueue` holds 256 of them, so a byte here is 256 bytes of resident memory.
inline constexpr std::size_t kInputEventSizeBudget = 320;
static_assert(sizeof(InputEvent) <= kInputEventSizeBudget,
              "ui::InputEvent grew past kInputEventSizeBudget");

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
        return producer_.overflowed.load(std::memory_order_relaxed);
    }
    void clear_overflow() {
        producer_.overflowed.store(false, std::memory_order_relaxed);
    }

private:
    // The ring indices are split from each other by a cache line (security review finding C2). The
    // queue is a single-producer/single-consumer ring: the producer writes `tail`, the consumer
    // writes `head`, and `overflowed` is producer-only. Declared consecutively they shared one
    // 64-byte line, so every push invalidated the line the consumer was reading. Today every
    // producer in the tree runs on the UI thread, so this is latent rather than live - but
    // docs/design/input.md section 7 and ADR-017 anticipate a platform callback thread pushing
    // input, and the fix is free at construction time.
    struct ProducerIndices {
        std::atomic<std::size_t> tail{0};
        std::atomic<bool> overflowed{false};
    };
    struct ConsumerIndices {
        std::atomic<std::size_t> head{0};
    };

    std::vector<InputEvent> buffer_;
    std::size_t capacity_ = 0;
    // The two groups are cache-line aligned, so `InputQueue` itself is padded to a line boundary;
    // that padding is the point (see tessera/detail/cache_line.hpp).
    TESSERA_CACHE_LINE_BEGIN
    ::tessera::detail::CacheLine<ProducerIndices> producer_;
    ::tessera::detail::CacheLine<ConsumerIndices> consumer_;
    TESSERA_CACHE_LINE_END
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
