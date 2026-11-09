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
    Blur,
    // Boundary events fired when the hover target changes. `PointerOver`/`PointerOut` bubble from
    // their target down to (excluding) the deepest common ancestor of the old/new chains;
    // `PointerEnter`/`PointerLeave` do not bubble and fire once per node strictly below that
    // ancestor. Touch never produces them and pointer capture suppresses them (see input.md §7).
    PointerOver,
    PointerOut,
    PointerEnter,
    PointerLeave
};

enum class EventPhase { Capture, Target, Bubble };

// A scene-level event. `handled` and `prevent_default` are advisory flags: `handled` records that
// a handler consumed the event, `prevent_default` asks the dispatcher to skip its default action
// (V1: Tab focus traversal). Pointer fields are meaningful for pointer and wheel events; wheel
// deltas use the web sign (positive scroll_y scrolls content down). `pointer_kind` mirrors the
// normalized device kind and defaults to mouse, so handlers that do not care see the old behavior.
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
    PointerKind pointer_kind = PointerKind::Mouse;
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
