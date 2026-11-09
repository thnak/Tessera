// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace tessera::ui::text {

// Editing core (docs/specs/text-editing-v1.md section 12): a bounded UTF-8 buffer with a caret,
// an anchor (selection), and the editing commands the input/IME layer drives. No exceptions are
// thrown for editing errors; every command returns an EditResult value.
//
// Offsets are UTF-8 byte offsets and are always grapheme-cluster boundaries (commands snap them).
// The caret/anchor pair is stored as bytes; CaretPosition reports the equivalent code-point and
// grapheme indices for UI/accessibility consumers.
//
// Bounded length: at most `max_bytes` (default kMaxEditTextBytes = 64 KiB per ADR-033/G-01) UTF-8
// bytes. An insertion that would exceed the bound is truncated at a grapheme boundary (never inside
// a cluster) and the result reports `truncated`; the buffer never exceeds the bound. Growth is
// std::string's amortized growth; callers may `reserve` once and then edit without allocation until
// the bound is reached.
inline constexpr std::size_t kMaxEditTextBytes = 64 * 1024;

using EditOffset = std::size_t;

// A caret position in all three units the editing/UI layers use. `bytes` is authoritative; the
// indices are counted from the start of the buffer.
struct CaretPosition {
    EditOffset bytes = 0;
    std::uint32_t codepoints = 0;
    std::uint32_t graphemes = 0;
};

enum class EditStatus : std::uint8_t {
    Ok,          // the command changed the buffer/selection
    NoChange,    // the command was valid but there was nothing to do
    InvalidUtf8, // the input was not valid UTF-8; nothing changed
    OutOfRange,  // reserved for future commands (offsets are clamped today)
    Truncated,   // part of the input was dropped because of the byte bound
};

struct EditResult {
    EditStatus status = EditStatus::NoChange;
    bool changed = false;
    bool truncated = false;
    CaretPosition caret{};
    EditOffset anchor = 0;

    [[nodiscard]] bool ok() const {
        return status == EditStatus::Ok;
    }
};

// One editable text field's storage and selection. Commands are deterministic pure functions of the
// buffer state and their arguments; there is no clock, locale, or allocation beyond the buffer's
// own storage.
class EditBuffer {
public:
    explicit EditBuffer(std::size_t max_bytes = kMaxEditTextBytes);
    EditBuffer(std::string_view initial_text, std::size_t max_bytes = kMaxEditTextBytes);

    [[nodiscard]] std::string_view text() const {
        return text_;
    }
    [[nodiscard]] std::size_t size() const {
        return text_.size();
    }
    [[nodiscard]] bool empty() const {
        return text_.empty();
    }
    [[nodiscard]] std::size_t max_bytes() const {
        return max_bytes_;
    }
    // Current allocated capacity (never larger than max_bytes + the string's growth granularity).
    [[nodiscard]] std::size_t capacity() const {
        return text_.capacity();
    }
    // Reserves storage (bounded by max_bytes); returns false when the request is rejected.
    bool reserve(std::size_t bytes);

    // Replaces the whole content (normalized CRLF -> LF, truncated at the byte bound). Resets the
    // selection to the end; invalid UTF-8 leaves the buffer unchanged.
    EditResult set_text(std::string_view text);

    [[nodiscard]] EditOffset caret() const {
        return caret_;
    }
    [[nodiscard]] EditOffset anchor() const {
        return anchor_;
    }
    [[nodiscard]] bool has_selection() const {
        return caret_ != anchor_;
    }
    [[nodiscard]] EditOffset selection_start() const {
        return caret_ < anchor_ ? caret_ : anchor_;
    }
    [[nodiscard]] EditOffset selection_end() const {
        return caret_ < anchor_ ? anchor_ : caret_;
    }
    [[nodiscard]] std::string_view selected_text() const;
    [[nodiscard]] CaretPosition caret_position() const;

    // Selection/caret commands. Offsets clamp to [0, size] and snap down to a cluster boundary.
    EditResult set_caret(EditOffset offset);
    EditResult set_selection(EditOffset anchor, EditOffset focus);
    EditResult select_all();
    EditResult clear_selection();

    // Text commands. `insert` replaces the current selection (a collapsed selection is an
    // insertion at the caret) and leaves the caret after the inserted text.
    EditResult insert(std::string_view text);
    // Replaces [start, end) explicitly (used for composition commits); the caret ends after the
    // replacement. Offsets clamp and snap; swapped when end < start.
    EditResult replace_range(EditOffset start, EditOffset end, std::string_view text);

    EditResult delete_backward(bool by_word = false);
    EditResult delete_forward(bool by_word = false);

    // Movement. `extend` moves the focus (caret) and keeps the anchor; without it a non-collapsed
    // selection collapses to the leading edge for left/right movement.
    EditResult move_left(bool by_word = false, bool extend = false);
    EditResult move_right(bool by_word = false, bool extend = false);
    EditResult move_home(bool document = false, bool extend = false);
    EditResult move_end(bool document = false, bool extend = false);

private:
    EditOffset snap(EditOffset offset) const;
    EditResult snapshot(EditStatus status, bool changed, bool truncated = false) const;
    void clamp_selection();

    std::string text_;
    EditOffset caret_ = 0;
    EditOffset anchor_ = 0;
    std::size_t max_bytes_ = kMaxEditTextBytes;
};

} // namespace tessera::ui::text
