// SPDX-License-Identifier: MIT
//
// Pure Android IME translation (issue #189, docs/design/mobile-integration.md section 3.3).
//
// No Android SDK, no <jni.h>, no device: the mapping takes UTF-16 code units and offsets and
// produces `ui::InputEvent`. The two conversions that matter (UTF-16 to UTF-8, and UTF-16 offsets
// to UTF-8 byte offsets) are here rather than in the bridge so a unit test can pin them.

#include "android_ime_translation.hpp"

namespace tessera::hal::android {
namespace {

constexpr char32_t kReplacementCharacter = 0xFFFD;

// Decodes one UTF-16 sequence into a scalar plus the number of units it used. A high surrogate
// followed by a low one is a pair; a lone surrogate is the replacement character, one unit.
char32_t decode_one(const char16_t* text, std::size_t length, std::size_t index,
                    std::uint8_t& units) noexcept {
    const char16_t first = text[index];
    if (first >= 0xD800 && first <= 0xDBFF && index + 1 < length) {
        const char16_t second = text[index + 1];
        if (second >= 0xDC00 && second <= 0xDFFF) {
            units = 2;
            const char32_t high = static_cast<char32_t>(first - 0xD800);
            const char32_t low = static_cast<char32_t>(second - 0xDC00);
            return 0x10000 + ((high << 10) | low);
        }
    }
    units = 1;
    if (first >= 0xD800 && first <= 0xDFFF) {
        // A lone surrogate cannot be encoded, and dropping it would desynchronise every offset
        // after it, so it becomes the replacement character.
        return kReplacementCharacter;
    }
    return static_cast<char32_t>(first);
}

// UTF-8 length of a scalar.
std::uint8_t utf8_length(char32_t codepoint) noexcept {
    if (codepoint <= 0x7F) {
        return 1;
    }
    if (codepoint <= 0x7FF) {
        return 2;
    }
    if (codepoint <= 0xFFFF) {
        return 3;
    }
    return 4;
}

// Writes a scalar at `out`. The caller guarantees room for `utf8_length(codepoint)` bytes.
void encode_one(char32_t codepoint, char* out) noexcept {
    const std::uint8_t length = utf8_length(codepoint);
    switch (length) {
    case 1:
        out[0] = static_cast<char>(codepoint);
        break;
    case 2:
        out[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        out[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
        break;
    case 3:
        out[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        out[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
        break;
    default:
        out[0] = static_cast<char>(0xF0 | (codepoint >> 18));
        out[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
        break;
    }
}

} // namespace

Utf8Step utf8_step(const char16_t* text, std::size_t length) noexcept {
    if (text == nullptr || length == 0) {
        return {};
    }
    std::uint8_t units = 1;
    const char32_t codepoint = decode_one(text, length, 0, units);
    return Utf8Step{utf8_length(codepoint), units};
}

std::uint32_t utf8_offset_from_utf16(const char16_t* text, std::size_t length,
                                     std::int32_t utf16_offset) noexcept {
    if (text == nullptr || length == 0 || utf16_offset <= 0) {
        return 0;
    }
    std::uint32_t bytes = 0;
    std::size_t index = 0;
    while (index < length && static_cast<std::int32_t>(index) < utf16_offset) {
        std::uint8_t units = 1;
        const char32_t codepoint = decode_one(text, length, index, units);
        bytes += utf8_length(codepoint);
        index += units;
    }
    return bytes;
}

std::size_t copy_utf8_truncated(const char16_t* text, std::size_t length,
                                ui::InputEvent& out) noexcept {
    out.text_size = 0;
    out.text_truncated = false;
    if (text == nullptr || length == 0) {
        return 0;
    }
    std::size_t index = 0;
    while (index < length) {
        std::uint8_t units = 1;
        const char32_t codepoint = decode_one(text, length, index, units);
        const std::uint8_t needed = utf8_length(codepoint);
        if (out.text_size + needed > ui::kMaxEditTextBytes) {
            // Truncate at the sequence boundary: half a sequence is invalid UTF-8, and the engine
            // would render it as a replacement character it never received.
            out.text_truncated = true;
            break;
        }
        encode_one(codepoint, out.text.data() + out.text_size);
        out.text_size += needed;
        index += units;
    }
    return index;
}

ImeTranslation translate_ime_sample(const ImeSample& sample, bool& composing) noexcept {
    ImeTranslation result;
    ui::InputEvent& out = result.event;
    // The payload is bounded, so the event never allocates; the caps are the engine's own.
    out.text_size = 0;

    switch (sample.action) {
    case ImeAction::Composing: {
        // An empty composing update with no active composition is the platform clearing preedit; it
        // produces nothing rather than a start-then-cancel pair.
        const std::size_t copied = copy_utf8_truncated(sample.text, sample.text_length, out);
        if (copied == 0 && !composing) {
            return result;
        }
        out.type = composing ? ui::InputEventType::CompositionUpdate
                             : ui::InputEventType::CompositionStart;
        composing = true;
        break;
    }
    case ImeAction::Commit: {
        const std::size_t copied = copy_utf8_truncated(sample.text, sample.text_length, out);
        if (copied == 0) {
            // A commit of nothing is not an edit, but it DOES end an active composition (the IME
            // accepted what was already showing).
            if (composing) {
                composing = false;
                out.type = ui::InputEventType::CompositionEnd;
                break;
            }
            return result;
        }
        // A commit carries the committed text AND ends the composition: the engine inserts the text
        // and the preedit stops existing.
        out.type = ui::InputEventType::EditText;
        composing = false;
        break;
    }
    case ImeAction::Finish:
    case ImeAction::Cancel:
        if (!composing) {
            return result;
        }
        // Ending without text: the preedit is gone. Whether it was accepted is carried by `Commit`,
        // so this only stops the composition.
        out.type = ui::InputEventType::CompositionEnd;
        composing = false;
        break;
    case ImeAction::Delete: {
        if (sample.delete_before <= 0 && sample.delete_after <= 0) {
            return result;
        }
        // The editor's delete path is a key event, so a delete is reported the same way a hardware
        // Backspace would be. One event per unit keeps the editor's own history the single source
        // of undo steps instead of inventing a bulk delete.
        out.type = ui::InputEventType::KeyDown;
        out.key = sample.delete_before > 0 ? ui::key::kBackspace : ui::key::kDelete;
        break;
    }
    }

    // The caret, when the callback named one. Android reports UTF-16 units; the engine's selection
    // is UTF-8 byte offsets, and mixing them puts the caret in the wrong place on any non-ASCII
    // string.
    if (sample.cursor_utf16 >= 0) {
        const std::uint32_t offset =
            utf8_offset_from_utf16(sample.text, sample.text_length, sample.cursor_utf16);
        out.selection_start = offset;
        out.selection_end = offset;
    }

    result.translated = true;
    return result;
}

} // namespace tessera::hal::android
