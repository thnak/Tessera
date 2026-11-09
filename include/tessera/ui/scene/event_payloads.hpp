// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/event.hpp"

#include <cstdint>
#include <string_view>
#include <type_traits>

namespace tessera::ui {

// C++ records for the IR/schema event payload records (docs/design/compiler.md section 11 and
// docs/design/scene-graph.md section 6). The schema names (`ClickEvent`, `KeyEvent`, ...) are the
// IR payload names; the mapping table is:
//
//   PointerEvent     -> PointerEventInfo
//   ClickEvent       -> ClickEvent
//   KeyEvent         -> KeyEventInfo
//   FocusEvent       -> FocusEventInfo
//   TextChangeEvent  -> TextChangeEvent   (TextSubmitEvent = TextChangeEvent)
//   TextSelection    -> TextSelection
//   CompositionEvent -> CompositionEvent
//   ScrollEvent      -> ScrollEventInfo
//   ErrorInfo        -> ErrorInfo
//
// Everything here is trivially copyable and allocation-free. String fields are borrowed views into
// the `Event`'s storage (or empty), so a payload is only valid for the duration of the dispatch.
// Fields the scene `Event` does not carry yet are documented as zero/empty: `KeyEventInfo::code`
// is 0, `ScrollEventInfo` viewport/content sizes are 0, `ErrorInfo` is empty, and
// `TextChangeEvent::selection_*` come from the scene event's UTF-16 range.

struct PointerEventInfo {
    float x = 0.0f;
    float y = 0.0f;
    std::uint32_t pointer_id = 0;
    PointerKind pointer_kind = PointerKind::Mouse;
    PointerButton button = PointerButton::None;
    float pressure = 0.0f;
    double timestamp_s = 0.0;
    bool is_primary = false;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    bool meta = false;
};

struct ClickEvent {
    float x = 0.0f;
    float y = 0.0f;
    std::uint32_t pointer_id = 0;
    PointerKind pointer_kind = PointerKind::Mouse;
    PointerButton button = PointerButton::None;
    bool is_primary = false;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    bool meta = false;
};

struct KeyEventInfo {
    std::uint32_t key = 0;  // Unicode code point of the W3C `key` value in this slice
    std::uint32_t code = 0; // the scene event does not carry a physical code yet
    bool repeat = false;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;
    bool meta = false;
};

struct FocusEventInfo {
    bool operator==(const FocusEventInfo&) const = default;
};

struct TextChangeEvent {
    std::string_view value;        // borrowed from Event::text; valid during dispatch only
    std::string_view target_value; // the Tessera form: equal to `value`
    std::uint32_t selection_start = 0;
    std::uint32_t selection_end = 0;
};

using TextSubmitEvent = TextChangeEvent;

struct TextSelection {
    std::uint32_t anchor = 0;
    std::uint32_t focus = 0;
};

struct CompositionEvent {
    std::string_view text; // borrowed from Event::text; valid during dispatch only
    std::uint32_t start = 0;
    std::uint32_t end = 0;
};

struct ScrollEventInfo {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float viewport_width = 0.0f;
    float viewport_height = 0.0f;
    float content_width = 0.0f;
    float content_height = 0.0f;
};

struct ErrorInfo {
    std::string_view code;
    std::string_view message;
};

constexpr bool modifier_held(Modifier value, Modifier flag) {
    return (static_cast<std::uint8_t>(value) & static_cast<std::uint8_t>(flag)) != 0;
}

inline std::string_view event_text(const Event& event) {
    return std::string_view(event.text.data(), event.text_size);
}

inline PointerEventInfo pointer_event_info(const Event& event) {
    return PointerEventInfo{event.x,
                            event.y,
                            event.pointer_id,
                            event.pointer_kind,
                            event.button,
                            event.pressure,
                            event.timestamp_s,
                            event.is_primary,
                            modifier_held(event.modifiers, Modifier::Shift),
                            modifier_held(event.modifiers, Modifier::Control),
                            modifier_held(event.modifiers, Modifier::Alt),
                            modifier_held(event.modifiers, Modifier::Meta)};
}

inline ClickEvent click_event(const Event& event) {
    return ClickEvent{event.x,
                      event.y,
                      event.pointer_id,
                      event.pointer_kind,
                      event.button,
                      event.is_primary,
                      modifier_held(event.modifiers, Modifier::Shift),
                      modifier_held(event.modifiers, Modifier::Control),
                      modifier_held(event.modifiers, Modifier::Alt),
                      modifier_held(event.modifiers, Modifier::Meta)};
}

inline KeyEventInfo key_event_info(const Event& event) {
    return KeyEventInfo{event.key,
                        0,
                        event.repeat,
                        modifier_held(event.modifiers, Modifier::Shift),
                        modifier_held(event.modifiers, Modifier::Control),
                        modifier_held(event.modifiers, Modifier::Alt),
                        modifier_held(event.modifiers, Modifier::Meta)};
}

inline FocusEventInfo focus_event_info(const Event&) {
    return FocusEventInfo{};
}

inline TextChangeEvent text_change_event(const Event& event) {
    const std::string_view value = event_text(event);
    return TextChangeEvent{value, value, event.selection_start, event.selection_end};
}

inline TextSubmitEvent text_submit_event(const Event& event) {
    return text_change_event(event);
}

inline TextSelection text_selection_event(const Event& event) {
    return TextSelection{event.selection_start, event.selection_end};
}

inline CompositionEvent composition_event(const Event& event) {
    return CompositionEvent{event_text(event), 0, 0};
}

inline ScrollEventInfo scroll_event_info(const Event& event) {
    return ScrollEventInfo{event.scroll_x, event.scroll_y, 0.0f, 0.0f, 0.0f, 0.0f};
}

inline ErrorInfo error_info(const Event&) {
    return ErrorInfo{};
}

static_assert(std::is_trivially_copyable_v<PointerEventInfo>);
static_assert(std::is_trivially_copyable_v<ClickEvent>);
static_assert(std::is_trivially_copyable_v<KeyEventInfo>);
static_assert(std::is_trivially_copyable_v<FocusEventInfo>);
static_assert(std::is_trivially_copyable_v<TextChangeEvent>);
static_assert(std::is_trivially_copyable_v<TextSelection>);
static_assert(std::is_trivially_copyable_v<CompositionEvent>);
static_assert(std::is_trivially_copyable_v<ScrollEventInfo>);
static_assert(std::is_trivially_copyable_v<ErrorInfo>);

} // namespace tessera::ui
