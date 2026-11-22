// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/input.hpp"
#include "tessera/ui/key_code.hpp"

#include <cstdint>
#include <string_view>

// Pure translation of the Android IME callbacks into `ui::InputEvent` (issue #189,
// docs/design/mobile-integration.md section 3.3).
//
// A custom-rendered text field does not get an IME for free: the platform talks to an
// `InputConnection`, and the engine's edit controller consumes `ui::InputEvent`. This header is the
// mapping between the two, and it is pure over plain strings and offsets so it compiles and runs on
// every lane with no Android SDK, no `<jni.h>` and no device.
//
// The engine already models composition: `CompositionStart`/`Update`/`End` carry the preedit and
// `EditText` carries a committed string (see `ui::InputEvent` and `ui::text::EditController`). So
// the bridge does not need a new engine seam — it needs to produce the events the engine already
// reads.
//
// Two conversions are the whole reason this is a separate testable unit:
//
//   1. UTF-16 to UTF-8. Android strings are UTF-16 and `ui::InputEvent::text` is UTF-8 with a
//      256-byte cap. A naive per-character loop splits a surrogate pair and produces a lone
//      replacement character, which shows up as a mojibake box for every emoji.
//   2. UTF-16 offsets to UTF-8 byte offsets. Android reports the caret in UTF-16 units; the
//   engine's
//      `selection_start`/`selection_end` are UTF-8 byte offsets into the same text. Mixing the two
//      puts the caret in the wrong place on every non-ASCII string.

namespace tessera::hal::android {

// The IME callbacks that map onto an input event. `Composing` is `setComposingText`, `Commit` is
// `commitText`, `Finish` is `finishComposingText` with no text, `Cancel` is a composition abandoned
// by the platform, and `Delete` is `deleteSurroundingText`.
enum class ImeAction : std::uint8_t {
    Composing,
    Commit,
    Finish,
    Cancel,
    Delete,
};

// One `InputConnection` callback, reduced to what the mapping needs.
struct ImeSample {
    ImeAction action = ImeAction::Composing;
    // The callback's text, as UTF-16 code units. Android hands the bridge a `CharSequence`; the
    // bridge reads it into this buffer once, because the mapping is pure and must not call back
    // into Java.
    static constexpr std::size_t kMaxUtf16 = 512;
    char16_t text[kMaxUtf16]{};
    std::size_t text_length = 0;
    // The new cursor position, in UTF-16 units relative to the text above. `-1` means "leave the
    // caret alone", which is what `commitText` with a null position means.
    std::int32_t cursor_utf16 = -1;
    // For `Delete`: how many UTF-16 units before and after the caret to remove.
    std::int32_t delete_before = 0;
    std::int32_t delete_after = 0;
};

// What one callback means for the queue. `translated` is false when the callback produces nothing
// (an empty composing update, a delete of zero units).
struct ImeTranslation {
    ui::InputEvent event;
    bool translated = false;
};

// UTF-8 length of one UTF-16 code unit sequence: 1 for ASCII, 2 for U+0080..U+07FF, 3 for the BMP
// above that, and 4 for a surrogate pair. `units` is how many UTF-16 units the sequence consumed,
// so a caller walking a string can advance correctly even at a lone surrogate.
//
// A lone surrogate is encoded as U+FFFD (3 bytes, 1 unit) rather than skipped: dropping it would
// make the byte offset disagree with the unit offset for everything after it.
struct Utf8Step {
    std::uint8_t bytes = 0;
    std::uint8_t units = 1;
};
[[nodiscard]] Utf8Step utf8_step(const char16_t* text, std::size_t length) noexcept;

// The UTF-8 byte offset of a UTF-16 offset, clamped to the string's UTF-8 length. This is what
// turns Android's caret into the engine's selection.
[[nodiscard]] std::uint32_t utf8_offset_from_utf16(const char16_t* text, std::size_t length,
                                                   std::int32_t utf16_offset) noexcept;

// Fills `out.text`/`out.text_size` from UTF-16, truncating at a UTF-8 sequence boundary and setting
// `text_truncated` when it does not fit. Returns the number of UTF-16 units consumed, so a caller
// can tell a full copy from a truncated one.
[[nodiscard]] std::size_t copy_utf8_truncated(const char16_t* text, std::size_t length,
                                              ui::InputEvent& out) noexcept;

// Converts one callback. A `Composing` callback becomes `CompositionStart` when no composition is
// active and `CompositionUpdate` while one is; `Commit` becomes `EditText` (which the edit
// controller inserts) and ends the composition; `Finish`/`Cancel` become `CompositionEnd`; `Delete`
// becomes a `KeyDown` of Backspace/Delete per unit, because that is the editor's delete path.
//
// `composing` is the caller's composition state, updated in place. Keeping it here rather than in
// the bridge is what lets a test drive a full IME session: compose, update, commit.
[[nodiscard]] ImeTranslation translate_ime_sample(const ImeSample& sample,
                                                  bool& composing) noexcept;

} // namespace tessera::hal::android
