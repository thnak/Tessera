// SPDX-License-Identifier: MIT
//
// Pure Android IME translation (issue #189): no Android SDK, no JNI, no device.
//
// What it proves is what a device lane cannot see cheaply: that a surrogate pair survives the
// UTF-16 to UTF-8 conversion, and that a caret reported in UTF-16 units lands on the right UTF-8
// byte. Both fail silently on screen (a mojibake box, a caret in the wrong place) and both are
// cheap to pin here.

#include "android_ime_translation.hpp"

#include <iostream>
#include <string_view>

namespace {

namespace android = tessera::hal::android;
using tessera::ui::InputEventType;

int failures = 0;

void check(bool condition, const char* name) {
    if (!condition) {
        std::cerr << "FAIL " << name << "\n";
        ++failures;
    }
}

// Fills a sample from an ASCII string, the common case.
android::ImeSample ascii_sample(android::ImeAction action, std::string_view text) {
    android::ImeSample sample;
    sample.action = action;
    for (std::size_t i = 0; i < text.size() && i < android::ImeSample::kMaxUtf16; ++i) {
        sample.text[i] = static_cast<char16_t>(static_cast<unsigned char>(text[i]));
        ++sample.text_length;
    }
    return sample;
}

// The UTF-8 bytes an event carries, as a string_view into the event's own buffer.
std::string_view text_of(const tessera::ui::InputEvent& event) {
    return std::string_view(event.text.data(), event.text_size);
}

} // namespace

int main() {
    // --- ASCII --------------------------------------------------------------------------------
    // "abc" is three 1-byte sequences, so the byte count equals the unit count.
    {
        android::ImeSample sample = ascii_sample(android::ImeAction::Composing, "abc");
        const android::Utf8Step step = android::utf8_step(sample.text, sample.text_length);
        check(step.bytes == 1, "an ASCII character is one byte");
        check(step.units == 1, "and one UTF-16 unit");
        check(android::utf8_offset_from_utf16(sample.text, sample.text_length, 2) == 2,
              "an ASCII offset is the same in bytes and units");
    }

    // --- Two-byte (Latin-1 supplement, e.g. e-acute) ------------------------------------------
    {
        android::ImeSample sample;
        sample.action = android::ImeAction::Composing;
        sample.text[0] = 0x00E9; // U+00E9
        sample.text_length = 1;
        const android::Utf8Step step = android::utf8_step(sample.text, sample.text_length);
        check(step.bytes == 2, "U+00E9 is two UTF-8 bytes");
        check(step.units == 1, "and one UTF-16 unit");
    }

    // --- Three-byte (CJK) ---------------------------------------------------------------------
    {
        android::ImeSample sample;
        sample.action = android::ImeAction::Composing;
        sample.text[0] = 0x4E2D; // the CJK character for "middle"
        sample.text_length = 1;
        const android::Utf8Step step = android::utf8_step(sample.text, sample.text_length);
        check(step.bytes == 3, "a CJK character is three UTF-8 bytes");
        check(step.units == 1, "and one UTF-16 unit");
    }

    // --- Four-byte (surrogate pair) -----------------------------------------------------------
    // This is the conversion the test exists for: a naive per-char loop encodes each half
    // separately and produces two replacement characters.
    {
        android::ImeSample sample;
        sample.action = android::ImeAction::Composing;
        sample.text[0] = 0xD83D; // high surrogate
        sample.text[1] = 0xDE00; // low surrogate -> U+1F600
        sample.text_length = 2;
        const android::Utf8Step step = android::utf8_step(sample.text, sample.text_length);
        check(step.bytes == 4, "a non-BMP character is four UTF-8 bytes");
        check(step.units == 2, "and consumes both surrogate halves");
        // The offset after the pair is 4 bytes, not 2: the caret must follow the encoded length.
        check(android::utf8_offset_from_utf16(sample.text, sample.text_length, 2) == 4,
              "the offset after a surrogate pair is four bytes");
    }

    // --- A lone surrogate is the replacement character, not a skip ----------------------------
    // Dropping it would desynchronise every offset after it, so it is encoded as U+FFFD.
    {
        android::ImeSample sample;
        sample.action = android::ImeAction::Composing;
        sample.text[0] = 0xD83D; // an unpaired high surrogate
        sample.text_length = 1;
        const android::Utf8Step step = android::utf8_step(sample.text, sample.text_length);
        check(step.bytes == 3, "a lone surrogate becomes the three-byte replacement character");
        check(step.units == 1, "and consumes one unit");
    }

    // --- Mixed string offsets -----------------------------------------------------------------
    // "a" (1 byte) + emoji (4 bytes) + "b" (1 byte): the offset after the emoji is 5, not 3.
    {
        android::ImeSample sample;
        sample.action = android::ImeAction::Composing;
        sample.text[0] = u'a';
        sample.text[1] = 0xD83D;
        sample.text[2] = 0xDE00;
        sample.text[3] = u'b';
        sample.text_length = 4;
        check(android::utf8_offset_from_utf16(sample.text, sample.text_length, 3) == 5,
              "the offset after an emoji counts its four bytes");
    }

    // --- Truncation at a sequence boundary ----------------------------------------------------
    // A payload larger than the event's buffer must be cut between sequences, never inside one.
    {
        android::ImeSample sample;
        sample.action = android::ImeAction::Composing;
        // Fill with 200 emoji (800 bytes) so the 256-byte cap bites.
        std::size_t units = 0;
        for (std::size_t i = 0; i < 200 && units + 2 <= android::ImeSample::kMaxUtf16; ++i) {
            sample.text[units++] = 0xD83D;
            sample.text[units++] = 0xDE00;
        }
        sample.text_length = units;
        tessera::ui::InputEvent event;
        const std::size_t consumed =
            android::copy_utf8_truncated(sample.text, sample.text_length, event);
        check(event.text_truncated, "an oversized payload is marked truncated");
        check(event.text_size <= tessera::ui::kMaxEditTextBytes, "the payload fits the buffer");
        // 4 bytes per emoji, so 64 emoji fit in 256 bytes exactly.
        check(event.text_size == 256, "the cut is on a sequence boundary");
        check(consumed == 128, "128 UTF-16 units were consumed");
    }

    // --- A composing update starts the composition, and updates it after that -----------------
    {
        bool composing = false;
        const android::ImeTranslation first = android::translate_ime_sample(
            ascii_sample(android::ImeAction::Composing, "ab"), composing);
        check(first.translated, "a first composing update is translated");
        check(first.event.type == InputEventType::CompositionStart,
              "with no active composition it starts one");
        check(text_of(first.event) == "ab", "and carries the preedit");
        check(composing, "the composition is now active");

        const android::ImeTranslation second = android::translate_ime_sample(
            ascii_sample(android::ImeAction::Composing, "abc"), composing);
        check(second.event.type == InputEventType::CompositionUpdate,
              "with an active composition it updates it");
        check(text_of(second.event) == "abc", "and carries the new preedit");
    }

    // --- An empty composing update with no composition produces nothing -----------------------
    // Otherwise a text field that has never been touched would emit a start-then-cancel pair on
    // every focus change.
    {
        bool composing = false;
        const android::ImeTranslation out = android::translate_ime_sample(
            ascii_sample(android::ImeAction::Composing, ""), composing);
        check(!out.translated, "an empty composing update with no composition is dropped");
        check(!composing, "and does not start one");
    }

    // --- A commit inserts the text and ends the composition -----------------------------------
    {
        bool composing = true;
        const android::ImeTranslation out = android::translate_ime_sample(
            ascii_sample(android::ImeAction::Commit, "abc"), composing);
        check(out.translated, "a commit is translated");
        check(out.event.type == InputEventType::EditText, "as an EditText the editor inserts");
        check(text_of(out.event) == "abc", "carrying the committed text");
        check(!composing, "and it ends the composition");
    }

    // --- A commit of nothing ends an active composition without inserting ---------------------
    {
        bool composing = true;
        const android::ImeTranslation out =
            android::translate_ime_sample(ascii_sample(android::ImeAction::Commit, ""), composing);
        check(out.translated, "an empty commit with an active composition is translated");
        check(out.event.type == InputEventType::CompositionEnd,
              "as the end of the composition, not an insert");
        check(out.event.text_size == 0, "carrying no text");
        check(!composing, "and the composition is over");
    }

    // --- An empty commit with no composition is a no-op ---------------------------------------
    {
        bool composing = false;
        const android::ImeTranslation out =
            android::translate_ime_sample(ascii_sample(android::ImeAction::Commit, ""), composing);
        check(!out.translated, "an empty commit with no composition is dropped");
    }

    // --- Finish and cancel end the composition ------------------------------------------------
    {
        bool composing = true;
        const android::ImeTranslation finished =
            android::translate_ime_sample(ascii_sample(android::ImeAction::Finish, ""), composing);
        check(finished.event.type == InputEventType::CompositionEnd, "finish ends the composition");
        check(!composing, "and clears the flag");
    }
    {
        bool composing = true;
        const android::ImeTranslation cancelled =
            android::translate_ime_sample(ascii_sample(android::ImeAction::Cancel, ""), composing);
        check(cancelled.event.type == InputEventType::CompositionEnd, "cancel ends it too");
        check(!composing, "and clears the flag");
    }
    // Ending when nothing is composing produces nothing, so a stray finish is harmless.
    {
        bool composing = false;
        const android::ImeTranslation out =
            android::translate_ime_sample(ascii_sample(android::ImeAction::Finish, ""), composing);
        check(!out.translated, "a finish with no composition is dropped");
    }

    // --- Delete -------------------------------------------------------------------------------
    {
        android::ImeSample sample;
        sample.action = android::ImeAction::Delete;
        sample.delete_before = 1;
        bool composing = false;
        const android::ImeTranslation out = android::translate_ime_sample(sample, composing);
        check(out.event.type == InputEventType::KeyDown, "a delete is a key event");
        check(out.event.key == tessera::ui::key::kBackspace,
              "deleting before the caret is a backspace");
    }
    {
        android::ImeSample sample;
        sample.action = android::ImeAction::Delete;
        sample.delete_after = 1;
        bool composing = false;
        const android::ImeTranslation out = android::translate_ime_sample(sample, composing);
        check(out.event.key == tessera::ui::key::kDelete,
              "deleting after the caret is a forward delete");
    }
    // A delete of zero units produces nothing, so a no-op callback does not reach the editor.
    {
        android::ImeSample sample;
        sample.action = android::ImeAction::Delete;
        bool composing = false;
        const android::ImeTranslation out = android::translate_ime_sample(sample, composing);
        check(!out.translated, "a delete of nothing is dropped");
    }

    // --- The caret is a UTF-8 offset ----------------------------------------------------------
    // "ab" + emoji, caret after the emoji: Android says 4 UTF-16 units, the engine needs 6 bytes.
    {
        android::ImeSample sample;
        sample.action = android::ImeAction::Commit;
        sample.text[0] = u'a';
        sample.text[1] = u'b';
        sample.text[2] = 0xD83D;
        sample.text[3] = 0xDE00;
        sample.text_length = 4;
        sample.cursor_utf16 = 4;
        bool composing = false;
        const android::ImeTranslation out = android::translate_ime_sample(sample, composing);
        check(out.event.selection_start == 6, "the caret is a UTF-8 byte offset");
        check(out.event.selection_end == 6, "and the selection is collapsed there");
    }
    // -1 means "leave the caret alone", which is what commitText with a null position means.
    {
        android::ImeSample sample = ascii_sample(android::ImeAction::Commit, "abc");
        sample.cursor_utf16 = -1;
        bool composing = false;
        const android::ImeTranslation out = android::translate_ime_sample(sample, composing);
        check(out.event.selection_start == 0 && out.event.selection_end == 0,
              "no cursor leaves the selection at its default");
    }
    // An offset past the end clamps rather than reading past the string.
    {
        android::ImeSample sample = ascii_sample(android::ImeAction::Commit, "abc");
        sample.cursor_utf16 = 99;
        bool composing = false;
        const android::ImeTranslation out = android::translate_ime_sample(sample, composing);
        check(out.event.selection_start == 3, "an out-of-range caret clamps to the end");
    }

    if (failures != 0) {
        std::cerr << "android_ime: " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "android_ime: ok\n";
    return 0;
}
