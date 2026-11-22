// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/scene/event_type.hpp"

#include "tessera/ui/input.hpp"

#include <cstdint>
#include <string_view>

namespace tessera::ui {

// A scene-level event. `handled` and `prevent_default` are advisory flags: `handled` records that
// a handler consumed the event, `prevent_default` asks the dispatcher to skip its default action
// (V1: Tab focus traversal, the wheel scroll). Pointer fields are meaningful for pointer and wheel
// events; `scroll_x`/`scroll_y` are the platform wheel delta on a Wheel event and the applied
// scroll delta on a Scroll event. Both use the web sign (positive scroll_y scrolls content down).
// `pointer_kind` mirrors the normalized device kind and defaults to mouse, so handlers that do not
// care see the old behavior.
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
    // Meaningful for EventType::VolumeKey only.
    VolumeDirection volume_direction = VolumeDirection::Up;
    PointerButton button = PointerButton::None;
    std::uint32_t pointer_id = 0;
    PointerKind pointer_kind = PointerKind::Mouse;
    float scroll_x = 0;
    float scroll_y = 0;
    // Scroll payload, meaningful for EventType::Scroll only: the container's resulting offset and
    // the viewport/content metrics captured when the scene emitted the event. `scroll_event_info`
    // maps them to the generated `ScrollEventInfo` record. Zero for every other event type.
    float scroll_offset_x = 0;
    float scroll_offset_y = 0;
    float scroll_viewport_width = 0;
    float scroll_viewport_height = 0;
    float scroll_content_width = 0;
    float scroll_content_height = 0;
    float pressure = 0;
    double timestamp_s = 0;
    bool is_primary = false;
    bool prevent_default = false;
    // Meaningful for EventType::Click only.
    ClickSource click_source = ClickSource::Pointer;
    // IME/text payload: UTF-8 bytes plus the UTF-8 byte selection range. The bytes are BORROWED
    // (`text`/`text_size`), never copied: the producer keeps the storage alive for the synchronous
    // dispatch, which is the only time any handler runs. This used to be a 256-byte inline
    // `std::array<char, kMaxEditTextBytes>`, which every scene event paid for whether or not it
    // carried text - and events are stack-built in the input path and copied through
    // capture/bubble, so the buffer dominated the copy cost of a pointer move
    // (docs/architecture/god-type-audit-v1.md section 4.2).
    //
    // `text_truncated` stays meaningful for a producer that had to cut its payload; the scene no
    // longer truncates on its own because it no longer owns a buffer.
    const char* text = nullptr;
    std::uint32_t text_size = 0;
    bool text_truncated = false;
    // Half-open UTF-8 byte offsets: into the full text for `TextChange`, into the borrowed payload
    // for the IME/EditText events (the HAL converts the platform's UTF-16 caret).
    std::uint32_t selection_start = 0;
    std::uint32_t selection_end = 0;
    // Borrowed full-text view used by `TextChange`: points into the edit buffer and is only valid
    // for the duration of the dispatch. Historically this superseded the bounded inline copy; now
    // it is simply the unbounded variant of the borrowed payload above, kept because `TextChange`
    // carries the whole buffer rather than a truncated prefix.
    const char* text_view = nullptr;
    std::uint32_t text_view_size = 0;

    void stop_propagation() {
        stopped = true;
    }

    // Sets the borrowed text payload. The caller must keep `value`'s storage alive and unmoved
    // until the dispatch returns (an `InputEvent` member, a buffer that is not mutated during the
    // dispatch, or a string literal). Prefer this over assigning `text`/`text_size` by hand so the
    // truncation flag stays consistent.
    void set_text(std::string_view value) {
        text = value.data();
        text_size = static_cast<std::uint32_t>(value.size());
        text_truncated = false;
    }

    // The borrowed IME/committed-text payload, empty when the event carries none.
    [[nodiscard]] std::string_view text_bytes() const {
        return text == nullptr ? std::string_view{} : std::string_view(text, text_size);
    }
};

} // namespace tessera::ui
