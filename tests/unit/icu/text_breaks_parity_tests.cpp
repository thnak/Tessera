// SPDX-License-Identifier: MIT

// Parity matrix for the two break engines (task-101, docs/design/text.md section 6).
//
// Every intentional difference between the builtin reduced rules and the ICU (UAX #29 / #14)
// iterators is spelled out here; anything else must match, so a regression in the ICU wiring
// cannot silently change word navigation or wrapping. The navigation cases also pin the
// `edit_detail::word_left/word_right` contract that moved to the boundary sets.

#include "edit_grapheme.hpp"
#include "text_breaks.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::text::BreakBackend;
using tessera::ui::text::BreakBoundaries;
using tessera::ui::text::builtin_line_boundaries;
using tessera::ui::text::builtin_word_boundaries;
using tessera::ui::text::line_boundaries;
using tessera::ui::text::word_boundaries;
using tessera::ui::text::edit_detail::word_left;
using tessera::ui::text::edit_detail::word_right;

int failures = 0;

bool expect(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "text_breaks_parity: " << message << "\n";
        ++failures;
    }
    return condition;
}

std::string show(const std::vector<std::size_t>& offsets) {
    std::string out = "[";
    for (const std::size_t offset : offsets) {
        out += std::to_string(offset);
        out += ",";
    }
    if (out.size() > 1) {
        out.pop_back();
    }
    out += "]";
    return out;
}

std::string show(const BreakBoundaries& boundaries) {
    return show(boundaries.offsets);
}

bool same(const BreakBoundaries& lhs, const BreakBoundaries& rhs) {
    return lhs.offsets == rhs.offsets;
}

bool expect_same(std::string_view text, const BreakBoundaries& lhs, const BreakBoundaries& rhs,
                 std::string_view what) {
    if (lhs.offsets == rhs.offsets) {
        return true;
    }
    std::cerr << "text_breaks_parity: " << what << " mismatch on '" << text << "': builtin "
              << show(lhs) << " vs icu " << show(rhs) << "\n";
    ++failures;
    return false;
}

// Every offset must start a UTF-8 code point. A UTF-16 index leaking into a byte API would fail
// this immediately.
bool on_codepoints(std::string_view text, const BreakBoundaries& boundaries) {
    for (const std::size_t offset : boundaries.offsets) {
        if (offset > text.size()) {
            return false;
        }
        if (offset == text.size()) {
            continue;
        }
        if ((static_cast<unsigned char>(text[offset]) & 0xC0U) == 0x80U) {
            return false;
        }
    }
    return true;
}

bool contains_all(const BreakBoundaries& superset, const BreakBoundaries& subset) {
    for (const std::size_t offset : subset.offsets) {
        bool found = false;
        for (const std::size_t candidate : superset.offsets) {
            if (candidate == offset) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

// Pure-ASCII prose where the two engines must agree: the builtin word rule already matches UAX #29
// for letters, digits, underscores, and whitespace runs.
constexpr std::string_view kAsciiCorpus[] = {
    "",
    "a",
    "hello world",
    "foo bar baz",
    "  leading spaces",
    "trailing spaces   ",
    "line\nbreak",
    "snake_case name",
    "tab\tseparated\twords",
    "123 456",
};

void check_ascii_parity() {
    for (const std::string_view text : kAsciiCorpus) {
        const BreakBoundaries builtin = builtin_word_boundaries(text);
        const BreakBoundaries icu = word_boundaries(text);
        expect_same(text, builtin, icu, "word");
        expect(on_codepoints(text, builtin), "builtin word offsets land on code points");
        expect(on_codepoints(text, icu), "icu word offsets land on code points");
        expect(!builtin.offsets.empty() && builtin.offsets.front() == 0, "word set starts at 0");
        expect(text.empty() || builtin.offsets.back() == text.size(),
               "word set ends at the text size");
    }
}

void check_documented_word_differences() {
    // UAX #29 keeps an apostrophe inside a word (WB6/WB7); the builtin rule treats it as a
    // separator. This is the intentional navigation difference: Ctrl+Left over "don't" lands at
    // the start of the contraction with ICU and after the apostrophe without it.
    const std::string_view contraction = "don't stop";
    const BreakBoundaries builtin_contraction = builtin_word_boundaries(contraction);
    const BreakBoundaries icu_contraction = word_boundaries(contraction);
    expect(same(builtin_contraction, BreakBoundaries{{0, 3, 4, 5, 6, 10}, BreakBackend::Builtin}),
           std::string("builtin splits at the apostrophe: ") + show(builtin_contraction));
    expect(same(icu_contraction, BreakBoundaries{{0, 5, 6, 10}, BreakBackend::Icu}),
           std::string("icu keeps the contraction together: ") + show(icu_contraction));

    // UAX #29 keeps a decimal point inside a number (WB11/WB12).
    const std::string_view decimal = "3.14 is pi";
    expect(same(builtin_word_boundaries(decimal),
                BreakBoundaries{{0, 1, 2, 4, 5, 7, 8, 10}, BreakBackend::Builtin}),
           std::string("builtin splits the decimal: ") + show(builtin_word_boundaries(decimal)));
    expect(same(word_boundaries(decimal), BreakBoundaries{{0, 4, 5, 7, 8, 10}, BreakBackend::Icu}),
           std::string("icu keeps the number together: ") + show(word_boundaries(decimal)));

    // CJK has no spaces. ICU segments a Han run with the bundled CJK dictionary (你好|世界), which
    // is dictionary data and therefore always kept by the reduced package; the builtin rule treats
    // the run as one word. Ctrl+Left therefore moves one *word* inside a Chinese sentence with
    // ICU instead of jumping the whole run.
    const std::string_view cjk = "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C"; // 你好世界
    expect(same(builtin_word_boundaries(cjk), BreakBoundaries{{0, 12}, BreakBackend::Builtin}),
           std::string("builtin treats the CJK run as one word: ") +
               show(builtin_word_boundaries(cjk)));
    expect(same(word_boundaries(cjk), BreakBoundaries{{0, 6, 12}, BreakBackend::Icu}),
           std::string("icu segments the CJK run with the dictionary: ") +
               show(word_boundaries(cjk)));

    // Arabic splits at the space under both engines.
    const std::string_view arabic = "\xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 " // مرحبا
                                    "\xD8\xA8\xD8\xA7\xD9\x84\xD8\xB9\xD8\xA7\xD9\x84\xD9\x85";
    expect_same(arabic, builtin_word_boundaries(arabic), word_boundaries(arabic), "arabic word");
}

void check_line_opportunities() {
    // ICU's UAX #14 set is a superset of the builtin opportunities: the greedy wrap can only gain
    // break points, never lose one, so existing wraps stay legal.
    constexpr std::string_view corpus[] = {
        "hello world", "foo   bar", "co-operate now", "path/to/file", "a b c", "hello\tworld",
    };
    for (const std::string_view text : corpus) {
        const BreakBoundaries builtin = builtin_line_boundaries(text);
        const BreakBoundaries icu = line_boundaries(text);
        expect(contains_all(icu, builtin),
               std::string("icu line set contains the builtin opportunities on '") +
                   std::string(text) + "': builtin " + show(builtin) + " icu " + show(icu));
        expect(on_codepoints(text, builtin), "builtin line offsets land on code points");
        expect(on_codepoints(text, icu), "icu line offsets land on code points");
    }

    // CJK: both engines break between ideographs, which is what keeps a CJK paragraph inside its
    // constraint instead of overflowing.
    const std::string_view cjk = "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C";
    expect(same(builtin_line_boundaries(cjk),
                BreakBoundaries{{0, 3, 6, 9, 12}, BreakBackend::Builtin}),
           std::string("builtin breaks CJK per ideograph: ") + show(builtin_line_boundaries(cjk)));
    expect(same(line_boundaries(cjk), BreakBoundaries{{0, 3, 6, 9, 12}, BreakBackend::Icu}),
           std::string("icu breaks CJK per ideograph: ") + show(line_boundaries(cjk)));
}

void check_navigation_contract() {
    // These are the word_left/word_right cases the editing layer relies on; they hold for the
    // builtin rule and must keep holding once ICU drives the boundaries.
    expect(word_right("foo bar", 0) == 3, "word_right from the start stops at the first word end");
    expect(word_right("foo bar", 3) == 7, "word_right from a space skips to the next word end");
    expect(word_right("foo   bar", 3) == 9, "word_right skips a whitespace run");
    expect(word_left("foo bar", 7) == 4, "word_left lands on the previous word start");
    expect(word_left("foo   bar", 6) == 0, "word_left skips the whitespace run backwards");
    expect(word_left("", 0) == 0, "word_left of an empty buffer is 0");
    expect(word_right("", 0) == 0, "word_right of an empty buffer is 0");
    expect(word_left("hello", 0) == 0, "word_left at the buffer start stays at 0");
    // Documented ICU difference: a CJK run moves one dictionary word at a time (你好|世界).
    expect(word_right("\xE4\xBD\xA0\xE5\xA5\xBD", 0) == 6,
           "word_right moves one CJK dictionary word with ICU");
}

void check_determinism() {
    const std::string_view text = "The quick brown fox \xE4\xBD\xA0\xE5\xA5\xBD 12.5% don't";
    const BreakBoundaries first = word_boundaries(text);
    const BreakBoundaries second = word_boundaries(text);
    expect(same(first, second), "word boundaries are deterministic");
    const BreakBoundaries line_first = line_boundaries(text);
    const BreakBoundaries line_second = line_boundaries(text);
    expect(same(line_first, line_second), "line boundaries are deterministic");
}

} // namespace

int main() {
    expect(tessera::ui::text::active_backend() == BreakBackend::Icu,
           "this build reports the ICU backend");
    check_ascii_parity();
    check_documented_word_differences();
    check_line_opportunities();
    check_navigation_contract();
    check_determinism();

    if (failures != 0) {
        std::cerr << "text_breaks_parity: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "text_breaks_parity: ok\n";
    return 0;
}
