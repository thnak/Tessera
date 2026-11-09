// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <cstdint>

namespace tessera::ui {

enum class EventType {
    PointerDown,
    PointerUp,
    PointerMove,
    Wheel,
    Text,
    KeyDown,
    KeyUp,
    Focus,
    Blur
};

enum class EventPhase { Capture, Target, Bubble };

// A scene-level event. `handled` and `prevent_default` are advisory flags: `handled` records that
// a handler consumed the event, `prevent_default` asks the dispatcher to skip its default action
// (V1: Tab focus traversal). Pointer fields are meaningful for pointer and wheel events; wheel
// deltas use the web sign (positive scroll_y scrolls content down).
struct Event {
    EventType type = EventType::PointerDown;
    float x = 0;
    float y = 0;
    EventPhase phase = EventPhase::Target;
    bool handled = false;
    bool stopped = false;
    std::uint32_t key = 0;
    Modifier modifiers = Modifier::None;
    std::uint32_t text_codepoint = 0;
    bool repeat = false;
    PointerButton button = PointerButton::None;
    std::uint32_t pointer_id = 0;
    float scroll_x = 0;
    float scroll_y = 0;
    float pressure = 0;
    double timestamp_s = 0;
    bool is_primary = false;
    bool prevent_default = false;

    void stop_propagation() {
        stopped = true;
    }
};

} // namespace tessera::ui
