// SPDX-License-Identifier: MIT
//
// EditBuffer: UTF-8 storage, caret/selection in byte/code-point/grapheme units, the documented
// simplified grapheme-cluster rule, CRLF normalization, word/line movement, clamping, and the
// bounded-length truncation rules.

#include "tessera/ui/text/edit_buffer.hpp"

#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::text::CaretPosition;
using tessera::ui::text::EditBuffer;
using tessera::ui::text::EditOffset;
using tessera::ui::text::EditStatus;
using tessera::ui::text::kMaxEditTextBytes;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_edit: " << message << "\n";
    }
    return condition;
}

bool at(const EditBuffer& buffer, EditOffset caret, std::uint32_t codepoints,
        std::uint32_t graphemes) {
    const CaretPosition position = buffer.caret_position();
    return position.bytes == caret && position.codepoints == codepoints &&
           position.graphemes == graphemes;
}

// "e" + U+0301 COMBINING ACUTE ACCENT: one cluster, two code points, three bytes.
constexpr std::string_view kDecomposedE = "e\xCC\x81";
// U+00E9: one cluster, one code point, two bytes.
constexpr std::string_view kComposedE = "\xC3\xA9";
// Family emoji: 1F468 ZWJ 1F469 ZWJ 1F467 ZWJ 1F466 - one ZWJ cluster, seven code points.
constexpr std::string_view kFamily = "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D"
                                     "\xF0\x9F\x91\xA7\xE2\x80\x8D\xF0\x9F\x91\xA6";
// U+2764 + U+FE0F VARIATION SELECTOR-16: one cluster, two code points.
constexpr std::string_view kRedHeart = "\xE2\x9D\xA4\xEF\xB8\x8F";
// Regional indicators JP: one flag cluster, two code points.
constexpr std::string_view kFlagJp = "\xF0\x9F\x87\xAF\xF0\x9F\x87\xB5";

bool units_and_caret() {
    EditBuffer buffer("a" + std::string(kComposedE) + "b");
    if (!expect(buffer.size() == 4, "composed e is two bytes") ||
        !expect(buffer.caret() == 4, "set_text puts the caret at the end") ||
        !expect(at(buffer, 4, 3, 3), "ascii+e+b is three code points and three clusters")) {
        return false;
    }
    EditBuffer decomposed{std::string(kDecomposedE)};
    return expect(decomposed.size() == 3, "decomposed e is three bytes") &&
           expect(at(decomposed, 3, 2, 1), "combining mark joins the cluster");
}

bool grapheme_deletion() {
    EditBuffer buffer{std::string(kFamily)};
    if (!expect(at(buffer, 25, 7, 1), "family emoji is one cluster") ||
        !expect(buffer.delete_backward().changed, "delete removes the family cluster") ||
        !expect(buffer.empty(), "ZWJ cluster deleted as a unit")) {
        return false;
    }
    EditBuffer heart{std::string(kRedHeart)};
    if (!expect(at(heart, 6, 2, 1), "variation selector joins the cluster") ||
        !expect(heart.delete_backward().changed && heart.empty(),
                "heart cluster deleted as a unit")) {
        return false;
    }
    EditBuffer flag{std::string(kFlagJp)};
    if (!expect(at(flag, 8, 2, 1), "regional indicator pair is one cluster") ||
        !expect(flag.delete_backward().changed && flag.empty(), "flag deleted as a unit")) {
        return false;
    }
    // Three regional indicators: one pair plus one single (two clusters).
    EditBuffer triple(std::string(kFlagJp) + "\xF0\x9F\x87\xBA");
    if (!expect(at(triple, 12, 3, 2), "third indicator starts its own cluster")) {
        return false;
    }
    if (!expect(triple.delete_backward().changed, "delete the odd indicator")) {
        return false;
    }
    return expect(triple.text() == kFlagJp, "the flag pair survives the first delete") &&
           expect(triple.delete_backward().changed && triple.empty(),
                  "second delete removes the pair");
}

bool crlf_and_validation() {
    EditBuffer buffer;
    if (!expect(buffer.insert("a\r\nb").ok(), "insert accepts CRLF") ||
        !expect(buffer.text() == "a\nb", "CRLF normalizes to LF") ||
        !expect(buffer.caret() == 3, "caret after the insert")) {
        return false;
    }
    if (!expect(buffer.insert("\r").ok() && buffer.text() == "a\nb\n",
                "lone CR normalizes to LF")) {
        return false;
    }
    if (!expect(buffer.insert("\x80").status == EditStatus::InvalidUtf8,
                "a bare continuation byte is rejected") ||
        !expect(buffer.text() == "a\nb\n", "invalid insert leaves the buffer unchanged") ||
        !expect(buffer.insert("\xC0\x80").status == EditStatus::InvalidUtf8,
                "overlong UTF-8 is rejected")) {
        return false;
    }
    EditBuffer other;
    return expect(other.set_text("\xFF").status == EditStatus::InvalidUtf8,
                  "invalid set_text is rejected") &&
           expect(other.empty(), "rejected set_text leaves the buffer empty");
}

bool insert_and_selection_replace() {
    EditBuffer buffer("hello world");
    if (!expect(buffer.set_caret(6).ok(), "caret set")) {
        return false;
    }
    if (!expect(buffer.set_selection(6, 11).ok() && buffer.has_selection(), "selection set") ||
        !expect(buffer.selected_text() == "world", "selected text view")) {
        return false;
    }
    const auto replaced = buffer.insert("there");
    return expect(replaced.ok() && replaced.changed, "replace selection") &&
           expect(buffer.text() == "hello there", "replacement text") &&
           expect(buffer.caret() == 11 && !buffer.has_selection(), "caret after the replacement") &&
           expect(buffer.insert("!").ok() && buffer.text() == "hello there!",
                  "insert at the caret");
}

bool word_rules() {
    EditBuffer buffer("hello world  foo");
    const EditOffset end = buffer.size();
    if (!expect(buffer.caret() == end, "caret at end after set_text")) {
        return false;
    }
    if (!expect(buffer.move_left(true, false).ok() && buffer.caret() == 13,
                "word-left lands on foo")) {
        return false;
    }
    if (!expect(buffer.move_left(true, false).ok() && buffer.caret() == 6,
                "word-left lands on world")) {
        return false;
    }
    if (!expect(buffer.move_right(true, false).ok() && buffer.caret() == 11,
                "word-right lands after world")) {
        return false;
    }
    if (!expect(buffer.move_right(true, false).ok() && buffer.caret() == 16,
                "word-right crosses the spaces to the end of foo")) {
        return false;
    }
    buffer.set_caret(end);
    const auto deleted = buffer.delete_backward(true);
    return expect(deleted.ok() && deleted.changed, "word delete changes") &&
           expect(buffer.text() == "hello world  ", "word delete removed foo") &&
           expect(buffer.caret() == 13, "word-delete caret");
}

bool line_movement() {
    EditBuffer buffer("abc\ndef\nghi");
    buffer.set_caret(5);
    if (!expect(buffer.caret() == 5, "caret inside line two")) {
        return false;
    }
    if (!expect(buffer.move_end(false, false).ok() && buffer.caret() == 7,
                "line end is the newline")) {
        return false;
    }
    if (!expect(buffer.move_home(false, false).ok() && buffer.caret() == 4,
                "line home is after the previous newline")) {
        return false;
    }
    if (!expect(buffer.move_home(true, false).ok() && buffer.caret() == 0, "document home")) {
        return false;
    }
    if (!expect(buffer.move_end(true, false).ok() && buffer.caret() == buffer.size(),
                "document end")) {
        return false;
    }
    buffer.set_caret(0);
    return expect(buffer.move_end(false, false).ok() && buffer.caret() == 3, "first line end");
}

bool selection_movement() {
    EditBuffer buffer("abcdef");
    buffer.set_selection(2, 5);
    if (!expect(buffer.move_left(false, false).ok() && buffer.caret() == 2 &&
                    !buffer.has_selection(),
                "plain left collapses to the selection start")) {
        return false;
    }
    buffer.set_selection(2, 5);
    if (!expect(buffer.move_right(false, false).ok() && buffer.caret() == 5 &&
                    !buffer.has_selection(),
                "plain right collapses to the selection end")) {
        return false;
    }
    buffer.set_caret(2);
    if (!expect(buffer.move_right(false, true).ok() && buffer.caret() == 3 &&
                    buffer.anchor() == 2 && buffer.has_selection(),
                "shift extends the selection")) {
        return false;
    }
    return expect(buffer.select_all().ok() && buffer.selection_start() == 0 &&
                      buffer.selection_end() == buffer.size(),
                  "select all");
}

bool clamp_rules() {
    EditBuffer buffer;
    if (!expect(buffer.move_left().status == EditStatus::NoChange, "left at the start") ||
        !expect(buffer.move_right().status == EditStatus::NoChange, "right at the end") ||
        !expect(buffer.delete_backward().status == EditStatus::NoChange, "backspace at start") ||
        !expect(buffer.delete_forward().status == EditStatus::NoChange, "delete at end")) {
        return false;
    }
    EditBuffer text{std::string(kComposedE)};
    if (!expect(text.set_caret(999).caret.bytes == text.size(), "caret clamps to the end") ||
        !expect(!text.set_caret(999).changed, "clamped caret reports no change")) {
        return false;
    }
    if (!expect(text.set_caret(1).ok() && text.caret() == 0,
                "caret snaps down to the cluster start")) {
        return false;
    }
    return expect(text.set_selection(1, 1).changed == false && text.selection_start() == 0,
                  "selection offsets snap too");
}

bool bounded_length() {
    EditBuffer buffer(5);
    const auto inserted = buffer.insert("abcdef");
    if (!expect(inserted.changed && inserted.truncated, "over-long insert truncates") ||
        !expect(buffer.text() == "abcde", "truncated at the byte bound") ||
        !expect(buffer.size() == 5, "never exceeds the bound")) {
        return false;
    }
    EditBuffer clusters(5);
    const auto clustered = clusters.insert(std::string(kComposedE) + std::string(kComposedE) +
                                           std::string(kComposedE));
    if (!expect(clustered.truncated && clusters.size() == 4,
                "cluster truncation keeps whole clusters") ||
        !expect(clusters.text() == std::string(kComposedE) + std::string(kComposedE),
                "no partial cluster is stored")) {
        return false;
    }
    EditBuffer small(2);
    if (!expect(small.insert("abc").truncated && small.text() == "ab", "small bound")) {
        return false;
    }
    EditBuffer huge;
    std::string large(kMaxEditTextBytes + 100, 'a');
    const auto big = huge.insert(large);
    return expect(big.truncated && huge.size() == kMaxEditTextBytes, "default bound is 64 KiB") &&
           expect(huge.reserve(kMaxEditTextBytes + 1) == false, "reserve above the bound fails");
}

bool reserve_and_capacity() {
    EditBuffer buffer;
    if (!expect(buffer.reserve(128), "reserve within the bound") ||
        !expect(buffer.capacity() >= 128, "capacity grew")) {
        return false;
    }
    const auto status = buffer.set_text("hello");
    return expect(status.ok() && !status.truncated, "set_text reports ok") &&
           expect(buffer.set_text("hello").status == EditStatus::NoChange,
                  "re-setting the same text is a no-op");
}

bool determinism_and_script() {
    const auto run = [](EditBuffer& buffer) {
        buffer.set_text("hello world");
        buffer.move_end(true, false);
        buffer.delete_backward(true);
        buffer.insert("there ");
        buffer.select_all();
        buffer.delete_backward();
        buffer.insert("done");
    };
    EditBuffer first;
    EditBuffer second;
    run(first);
    run(second);
    return expect(first.text() == "done", "script final text") &&
           expect(first.caret() == 4, "script final caret") &&
           expect(first.text() == second.text(), "identical text") &&
           expect(first.caret() == second.caret() && first.anchor() == second.anchor(),
                  "identical selection");
}

} // namespace

int main() {
    if (!units_and_caret()) {
        return 1;
    }
    if (!grapheme_deletion()) {
        return 2;
    }
    if (!crlf_and_validation()) {
        return 3;
    }
    if (!insert_and_selection_replace()) {
        return 4;
    }
    if (!word_rules()) {
        return 5;
    }
    if (!line_movement()) {
        return 6;
    }
    if (!selection_movement()) {
        return 7;
    }
    if (!clamp_rules()) {
        return 8;
    }
    if (!bounded_length()) {
        return 9;
    }
    if (!reserve_and_capacity()) {
        return 10;
    }
    if (!determinism_and_script()) {
        return 11;
    }

    std::cout << "text_edit: ok\n";
    return 0;
}
