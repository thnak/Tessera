// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

// Pure IME pre-edit (composition) state machine for the X11/XIM path. This header deliberately does
// not include <X11/Xlib.h>: the Xlib-facing pump mirrors XIMPreeditStartCallback/DrawCallback/Done
// into the calls below, so the composition contract is unit-testable without a display or an input
// method.
//
// The contract follows docs/specs/text-editing-v1.md section 4 and the Win32 producer:
// - CompositionStart/Update/End mirror the method's pre-edit lifecycle; committed text is one
//   EditText event, delivered before the CompositionEnd that closes the composition.
// - The payload is the bounded inline UTF-8 buffer on ui::InputEvent (at most kMaxPreeditBytes,
//   truncated on a UTF-8 sequence boundary with `text_truncated`), and the caret is reported as a
//   UTF-8 byte offset in `selection_start`/`selection_end`, the same unit as TextChange and the
//   Win32 producer's converted cursor.
// - Unbounded or malformed pre-edit data is clamped, never an error: invalid bytes become U+FFFD,
//   the caret clamps into the kept bytes, and a pre-edit longer than the bound is cut.

namespace tessera::hal::x11 {

// The pre-edit buffer is bounded by the normalized payload: committed text can never exceed what an
// EditText event can carry, so a commit always copies the whole composition without allocating.
inline constexpr std::size_t kMaxPreeditBytes = ui::kMaxEditTextBytes;

// At most one composition event pair: an EditText commit followed by CompositionEnd.
inline constexpr std::size_t kMaxCompositionEvents = 2;

enum class CompositionStatus : std::uint8_t { Idle, Composing };

struct CompositionOutput {
    std::array<ui::InputEvent, kMaxCompositionEvents> events{};
    std::size_t count = 0;
};

// Deterministic composition state machine. Every call is pure apart from the instance's own
// bounded buffer, so a recorded XIM callback sequence replays to an identical event list.
class CompositionState {
public:
    // Begins a composition from the method's UTF-8 pre-edit and caret character index
    // (XIMPreeditDrawCallbackStruct.caret). A second start without a commit replaces the
    // in-progress pre-edit and emits one CompositionStart for the new text: the replaced text is
    // never committed and no CompositionEnd is sent for it (documented in docs/design/input.md).
    [[nodiscard]] CompositionOutput start(std::string_view preedit_utf8,
                                          std::uint32_t caret_characters);
    // Updates the in-progress pre-edit. An update while idle starts the composition instead of
    // being lost (a method may draw before it announced the start).
    [[nodiscard]] CompositionOutput update(std::string_view preedit_utf8,
                                           std::uint32_t caret_characters);
    // Commits: one EditText with the composed text (omitted when the pre-edit is empty) followed by
    // CompositionEnd. A commit with no live composition is a no-op.
    [[nodiscard]] CompositionOutput commit();
    // Cancels: CompositionEnd only, the pre-edit is discarded and no text is committed. A cancel
    // with no live composition is a no-op.
    [[nodiscard]] CompositionOutput cancel();

    [[nodiscard]] bool composing() const {
        return composing_;
    }
    [[nodiscard]] std::string_view preedit() const {
        return {buffer_.data(), size_};
    }
    [[nodiscard]] std::uint32_t caret_bytes() const {
        return caret_bytes_;
    }
    // True when the last start/update cut the method's pre-edit at kMaxPreeditBytes.
    [[nodiscard]] bool truncated() const {
        return truncated_;
    }
    [[nodiscard]] CompositionStatus status() const {
        return composing_ ? CompositionStatus::Composing : CompositionStatus::Idle;
    }

    void reset();

private:
    void set_preedit(std::string_view text, std::uint32_t caret_characters);
    [[nodiscard]] ui::InputEvent payload_event(ui::InputEventType type) const;

    std::array<char, kMaxPreeditBytes> buffer_{};
    std::size_t size_ = 0;
    std::uint32_t caret_bytes_ = 0;
    bool truncated_ = false;
    bool composing_ = false;
};

// UTF-8 byte offset of character index `characters` in `text` (clamped to the text length). A
// malformed byte counts as one character, matching next_utf8_codepoint's one-byte U+FFFD recovery.
[[nodiscard]] std::uint32_t utf8_byte_offset(std::string_view text, std::uint32_t characters);

} // namespace tessera::hal::x11
