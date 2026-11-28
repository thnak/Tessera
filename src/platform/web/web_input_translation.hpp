// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <cstdint>
#include <string_view>

// Pure translation of DOM events into normalized `InputEvent`s (ADR-046 item 3). This header never
// includes a browser or Emscripten header: the host page (host/tessera_web_host.js) flattens each
// DOM event into the plain values below, so the mapping rules are unit-testable on any platform
// with no browser. UI code never sees a DOM object.

namespace tessera::hal::web {

// Bit layout the host page packs from `event.getModifierState`. The page and this header are the
// two halves of one contract, so the values are fixed and tested from both sides.
inline constexpr unsigned kModShift = 1U << 0;
inline constexpr unsigned kModControl = 1U << 1;
inline constexpr unsigned kModAlt = 1U << 2;
inline constexpr unsigned kModMeta = 1U << 3;
inline constexpr unsigned kModCapsLock = 1U << 4;
inline constexpr unsigned kModNumLock = 1U << 5;

// `PointerEvent.pointerType`, as the page encodes it (mouse 0, touch 1, pen 2). Anything else
// reaches the translator as Unknown and keeps the mouse behavior, so input is never dropped.
enum class DomPointerType : std::uint8_t { Mouse, Touch, Pen, Unknown };

// The four `pointerdown`/`pointermove`/`pointerup`/`pointercancel` phases.
enum class DomPointerPhase : std::uint8_t { Down, Move, Up, Cancel };

// `WheelEvent.deltaMode`: 0 pixels, 1 lines, 2 pages.
enum class DomWheelMode : std::uint8_t { Pixel, Line, Page, Unknown };

struct DomPointerSample {
    DomPointerPhase phase = DomPointerPhase::Move;
    DomPointerType type = DomPointerType::Unknown;
    std::uint32_t pointer_id = 0;
    bool primary = false;
    // `PointerEvent.button`: 0 left, 1 middle, 2 right, 3 back, 4 forward, -1 no button changed.
    int button = -1;
    // CSS pixels relative to the canvas (the page subtracts the bounding rect), which is the
    // logical unit `InputEvent` uses on every platform. Device pixel ratio never enters here.
    float x = 0.0F;
    float y = 0.0F;
    // `PointerEvent.pressure`, 0..1. Only a pen reports it into the event.
    float pressure = 0.0F;
    unsigned modifiers = 0;
    double timestamp_ms = 0.0;
};

struct DomWheelSample {
    float x = 0.0F;
    float y = 0.0F;
    float delta_x = 0.0F;
    float delta_y = 0.0F;
    DomWheelMode mode = DomWheelMode::Pixel;
    unsigned modifiers = 0;
    double timestamp_ms = 0.0;
};

struct DomKeySample {
    bool down = true;
    // `KeyboardEvent.key` as UTF-8 ("a", "Enter", "ArrowLeft", " ", "F5").
    std::string_view key;
    unsigned modifiers = 0;
    bool repeat = false;
    double timestamp_ms = 0.0;
};

// Wheel units per notch the page-side deltas are normalized by: browsers report ~100 px for a
// notch in pixel mode and 3 lines in line mode. A page counts as ten notches.
inline constexpr float kPixelsPerNotch = 100.0F;
inline constexpr float kLinesPerNotch = 3.0F;
inline constexpr float kNotchesPerPage = 10.0F;

[[nodiscard]] ui::Modifier modifiers_from_bits(unsigned bits);

// DOM button index to the normalized button. Unknown indexes are Other, -1 is None.
[[nodiscard]] ui::PointerButton button_from_dom(int dom_button);

// Layout-level key for a `KeyboardEvent.key` string, or kUnknown ("Dead", "Process", unmapped).
[[nodiscard]] ui::KeyCode key_code_from_dom(std::string_view key);

// The code point a key press types, or 0 when it types nothing: named keys, and chords held with
// Control or Meta (shortcuts, not text). Control together with Alt is AltGr on Windows keyboards
// and does type. Malformed UTF-8 yields U+FFFD.
[[nodiscard]] std::uint32_t typed_codepoint(std::string_view key, ui::Modifier modifiers);

[[nodiscard]] ui::InputEvent make_pointer_event(const DomPointerSample& sample);

// Wheel notches with the web sign (positive dy scrolls content down), like every native backend.
[[nodiscard]] ui::InputEvent make_wheel_event(const DomWheelSample& sample);

[[nodiscard]] ui::InputEvent make_key_event(const DomKeySample& sample);

// The `Text` event a key-down also produces, or false when it types nothing or is a key-up.
[[nodiscard]] bool make_text_event(const DomKeySample& sample, ui::InputEvent& out);

} // namespace tessera::hal::web
