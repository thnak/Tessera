// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>

// The event vocabulary of the scene graph, split out of `event.hpp` so a header that only needs to
// name an event kind (a binding table, a dispatch signature) does not pull in `input.hpp` and the
// whole `Event` payload. `event.hpp` includes this and adds the payload; nothing here depends on
// it, so the two can be included in either order.
//
// Why it matters: `node_metadata.hpp` stores `EventType` in its binding table and `node.hpp` is
// included by 80 translation units. Before this split, naming an event kind cost every one of them
// the ~41k preprocessed lines of `event.hpp` (measured: 0.33 s per TU).
namespace tessera::ui {

enum class EventType : std::uint8_t {
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
    // ancestor. Touch never produces them and pointer capture suppresses them (see input.md Â§7).
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
    TextChange,
    // Synthesized activation (issue #6): same-target pointer press and release, Enter/Space on a
    // focused button, or an accessibility invoke. Produced by the InputRouter, never by a
    // platform; `Event::click_source` records which path fired it. Bubbles like a pointer event.
    Click,
    // Emitted by the scene to a scroll container whose offset actually changed (wheel scrolling,
    // `scroll_to`/`scroll_by`, `scroll_into_view`) and dispatched in the normal
    // capture -> target -> bubble order. It is a scene event, not an OS event: the platform never
    // produces it, and a wheel event still precedes it (see wheel_dispatch.cpp). `scroll_x`/
    // `scroll_y` carry the delta the container applied with the web sign (positive `scroll_y`
    // scrolls content down), while `ScrollEventInfo` carries the resulting offsets and the
    // container's viewport/content metrics. `handled` is recorded normally; `prevent_default` is
    // advisory and cannot retroactively undo an applied scroll in V1 (pre-checking/clamping before
    // the move is a later slice). A handler that scrolls the same container again applies that move
    // and does not re-emit (bounded re-entrancy guard in scroll.cpp).
    Scroll,
    // A hardware volume key (docs/specs/studio-device-profiles-v1.md section 4), delivered to the
    // focused node in the normal capture -> target -> bubble order. The framework has no default
    // action for it: `volume_direction` carries which key, and an unhandled event is simply
    // dropped. BackRequested is not a scene event: the host resolves it against overlays and
    // navigation before the scene sees input.
    VolumeKey
};

enum class ClickSource : std::uint8_t { Pointer, Keyboard, Accessibility };

enum class EventPhase : std::uint8_t { Capture, Target, Bubble };

} // namespace tessera::ui
