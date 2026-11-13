// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/key_code.hpp"
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
//   TextChangeEvent  -> TextChangeEvent   (TextSubmitEvent = TextChangeEvent);
//                       delivered by EventType::TextChange through the typed adapter below
//   TextSelection    -> TextSelection
//   CompositionEvent -> CompositionEvent
//   ScrollEvent      -> ScrollEventInfo
//   SizeChangeEvent  -> SizeChangeEvent (delivered by layout observation, not an Event)
//   ErrorInfo        -> ErrorInfo
//
// Everything here is trivially copyable and allocation-free. String fields are borrowed views into
// the `Event`'s storage (or empty), so a payload is only valid for the duration of the dispatch.
// Fields the scene `Event` does not carry yet are documented as zero/empty: `KeyEventInfo::code`
// is 0, `ErrorInfo` is empty, and `TextChangeEvent::selection_*` come from the scene event's UTF-16
// range.

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
    // Where the activation came from; keyboard and accessibility clicks use the node center.
    ClickSource source = ClickSource::Pointer;
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

// A committed text-edit payload (docs/specs/text-editing-v1.md section 12). `value` is the new full
// UTF-8 text and is BORROWED: it stays valid only for the duration of the handler call (a generated
// handler that stores it must copy). `selection_start`/`selection_end` are the half-open selection
// as UTF-8 byte offsets (the units `PaintData::TextDecoration` consumes); the schema
// `TextSelection` row still says grapheme offsets and must be aligned in the schema slice.
struct TextChangeEvent {
    std::string_view value;            // borrowed from the edit buffer; valid during dispatch only
    std::string_view target_value;     // the Tessera form: equal to `value`
    std::uint32_t selection_start = 0; // half-open UTF-8 byte offsets
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

// Scene scroll payload (EventType::Scroll): `offset_*` are the container's absolute scroll offsets
// after the applied move; the viewport is the scrollport (border box inset by the container's
// `scrollport_insets`) and the content extents come from the node's content size. The applied delta
// itself is `Event::scroll_x`/`scroll_y`, not part of this record (the schema row has offsets
// only).
struct ScrollEventInfo {
    float offset_x = 0.0f;
    float offset_y = 0.0f;
    float viewport_width = 0.0f;
    float viewport_height = 0.0f;
    float content_width = 0.0f;
    float content_height = 0.0f;
};

// Layout-observation payload (docs/design/layout-observation.md section 5.5): border box and
// content box size in logical px. It is delivered by `LayoutObserverSet`, not through an `Event`.
struct SizeChangeEvent {
    float width = 0.0f;
    float height = 0.0f;
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
    if (event.text_view != nullptr) {
        return std::string_view(event.text_view, event.text_view_size);
    }
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
                      event.click_source,
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

// True when `info.key` is the W3C `key` value `name` ("Escape", "ArrowDown", " ", "a"). Generated
// code compares through this because the payload carries the logical key code (key_code.hpp), not
// the string. A single printable letter matches either case; an unknown name matches nothing.
inline bool key_is(const KeyEventInfo& info, std::string_view name) {
    struct Named {
        std::string_view name;
        std::uint32_t code;
    };
    constexpr Named kNamed[] = {
        {"Backspace", key::kBackspace},
        {"Tab", key::kTab},
        {"Enter", key::kEnter},
        {"Escape", key::kEscape},
        {" ", key::kSpace},
        {"Insert", key::kInsert},
        {"Delete", key::kDelete},
        {"Home", key::kHome},
        {"End", key::kEnd},
        {"PageUp", key::kPageUp},
        {"PageDown", key::kPageDown},
        {"ArrowLeft", key::kArrowLeft},
        {"ArrowUp", key::kArrowUp},
        {"ArrowRight", key::kArrowRight},
        {"ArrowDown", key::kArrowDown},
    };
    for (const Named& entry : kNamed) {
        if (entry.name == name) {
            return info.key == entry.code;
        }
    }
    if (name.size() == 1) {
        const char c = name[0];
        const std::uint32_t upper = (c >= 'a' && c <= 'z')
                                        ? static_cast<std::uint32_t>(c - 'a' + 'A')
                                        : static_cast<std::uint32_t>(c);
        return info.key == upper;
    }
    return false;
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

// Typed adapter for a generated handler `void on_change(const TextChangeEvent&)`.
//
// The node binding storage is `{function pointer, void* context}` with a generic
// `void(*)(void*, const Event&)` signature, so codegen never has to know about the trampoline: it
// declares one `TextChangeBinding` member per binding and calls `bind_text_change(node, binding)`.
// The binding object must outlive the node (or be unbound before the node is destroyed). The
// trampoline converts the scene `Event` to `TextChangeEvent` and calls the typed handler; the
// payload view is valid only for the duration of that call (see `TextChangeEvent`).
struct TextChangeBinding {
    void (*handler)(void* context, const TextChangeEvent& event) = nullptr;
    void* context = nullptr;

    [[nodiscard]] bool bound() const {
        return handler != nullptr;
    }
};

inline void text_change_trampoline(void* context, const Event& event) {
    auto* binding = static_cast<TextChangeBinding*>(context);
    if (binding == nullptr || binding->handler == nullptr || event.type != EventType::TextChange) {
        return;
    }
    binding->handler(binding->context, text_change_event(event));
}

// Binds a typed change handler on `node`. `NodeT` only needs the scene node's
// `bind_event(EventType, BoundEventHandler, void*)`; keeping this a template lets codegen include
// this header without pulling the node header in here.
template<typename NodeT> bool bind_text_change(NodeT& node, TextChangeBinding& binding) {
    return node.bind_event(EventType::TextChange, &text_change_trampoline, &binding);
}

inline TextSelection text_selection_event(const Event& event) {
    return TextSelection{event.selection_start, event.selection_end};
}

inline CompositionEvent composition_event(const Event& event) {
    return CompositionEvent{event_text(event), 0, 0};
}

inline ScrollEventInfo scroll_event_info(const Event& event) {
    return ScrollEventInfo{event.scroll_offset_x,       event.scroll_offset_y,
                           event.scroll_viewport_width, event.scroll_viewport_height,
                           event.scroll_content_width,  event.scroll_content_height};
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
