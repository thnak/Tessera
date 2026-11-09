// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <array>
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
    PointerLeave,
    // IME/text events, delivered to the focused node only. `EditText` carries committed
    // multi-codepoint text; the composition events carry the in-progress composition string.
    // `prevent_default` is advisory for these: there is no framework text insertion yet, so it is
    // a documented no-op until the text editor consumes it.
    CompositionStart,
    CompositionUpdate,
    CompositionEnd,
    EditText,
    // A committed text edit from an attached edit controller (docs/specs/text-editing-v1.md
    // section 12). One event per committed mutation (insert/replace/delete/backspace/delete or an
    // IME commit), never for preedit updates. `text_view`/`selection_*` below carry the payload;
    // generated `onChange` handlers bind through the typed adapter in event_payloads.hpp.
    TextChange
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
    // IME/text payload, mirrored from the normalized event: UTF-8 bytes plus the UTF-16
    // code-unit selection range. `text_truncated` marks a payload cut at `kMaxEditTextBytes`.
    std::array<char, kMaxEditTextBytes> text{};
    std::uint32_t text_size = 0;
    bool text_truncated = false;
    // For `TextChange` these are half-open UTF-8 byte offsets; for the IME/EditText events they are
    // UTF-16 code units, as the platform reports them.
    std::uint32_t selection_start = 0;
    std::uint32_t selection_end = 0;
    // Borrowed full-text view used by `TextChange`: points into the edit buffer and is only valid
    // for the duration of the dispatch (null on every other event type). When set it supersedes the
    // bounded inline `text` copy above, which recording sinks can still read.
    const char* text_view = nullptr;
    std::uint32_t text_view_size = 0;

    void stop_propagation() {
        stopped = true;
    }
};

} // namespace tessera::ui
