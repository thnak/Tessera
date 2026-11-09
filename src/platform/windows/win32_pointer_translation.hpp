// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <cstdint>

// Pure translation decisions for Win32 pointer and touch input. This header deliberately does not
// include <windows.h>: the Windows translator mirrors the fields it needs into the plain structs
// below, so the mapping rules are unit-testable without an HWND or a real touch/pen device.

namespace tessera::hal::win32 {

// Mirrors POINTER_INPUT_TYPE. Unknown covers future pointer types; see pointer_kind_of.
enum class PointerType : std::uint8_t {
    Mouse,
    Touch,
    Pen,
    Unknown,
};

// The subset of WM_POINTER* messages this translator maps.
enum class PointerMessage : std::uint8_t {
    Down,
    Update,
    Up,
    Cancel,
};

struct PointerSample {
    PointerType type = PointerType::Unknown;
    std::uint32_t pointer_id = 0;
    bool primary = false;
    // Raw POINTER_PEN_INFO.pressure (0..1024); ignored for mouse and touch.
    std::uint32_t pressure = 0;
    // Physical client coordinates (already ScreenToClient'ed by the caller).
    float client_x = 0.0f;
    float client_y = 0.0f;
    // Touch is only ever in contact while down; pen may report in-range hover (update, no contact).
    bool in_contact = true;
    ui::PointerButton button = ui::PointerButton::None;
    PointerMessage message = PointerMessage::Update;
};

struct TouchSample {
    std::uint32_t touch_id = 0; // TOUCHINPUT.dwID
    bool primary = false;
    bool down = false;
    bool up = false;
    bool move = false;
    // Physical client coordinates (converted from TOUCHINPUT's hundredths and ScreenToClient'ed).
    float client_x = 0.0f;
    float client_y = 0.0f;
};

// WM_TOUCH contacts map into the top half of the pointer-id space; WM_POINTER ids are used
// verbatim. The spaces are disjoint as long as Windows pointer ids stay below 0x80000000 (they are
// small handles in practice). Documented in docs/design/input.md section 2.1.
constexpr std::uint32_t kTouchPointerIdBase = 0x8000'0000u;

// Mouse for Mouse, Touch for Touch, Pen for Pen. Unknown/future pointer types map to Mouse so
// legacy or unrecognized input keeps the pre-kind behavior instead of being dropped.
[[nodiscard]] ui::PointerKind pointer_kind_of(PointerType type);

// Stable per-contact id for the WM_TOUCH fallback, disjoint from WM_POINTER ids.
[[nodiscard]] std::uint32_t touch_pointer_id(std::uint32_t touch_id);

// POINTER_PEN_INFO.pressure (0..1024) -> 0..1, clamped; 0 for touch/mouse callers.
[[nodiscard]] float normalize_pen_pressure(std::uint32_t raw_pressure);

// Fills `out` and returns true, or returns false when the sample must not produce an event:
// touch never hovers, so a touch update without contact is dropped. A cancel always maps.
[[nodiscard]] bool make_pointer_event(const PointerSample& sample, float dpi_scale,
                                      ui::InputEvent& out);

// Fills `out` and returns true for a contact down/move/up. Returns false when the TOUCHINPUT
// carries no event flag (nothing to synthesize, in particular no hover move).
[[nodiscard]] bool make_touch_event(const TouchSample& sample, float dpi_scale,
                                    ui::InputEvent& out);

} // namespace tessera::hal::win32
