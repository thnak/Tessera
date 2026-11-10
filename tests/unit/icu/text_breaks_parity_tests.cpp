// SPDX-License-Identifier: MIT

// Parity matrix for the two break engines (task-101, docs/design/text.md section 6).
//
// Every intentional difference between the builtin reduced rules and the ICU (UAX #29 / #14)
// iterators is spelled out here; anything else must match, so a regression in the ICU wiring
// cannot silently change word navigation or wrapping. The navigation cases also pin the
// `edit_detail::word_left/word_right` contract that moved to the boundary sets.

#include "edit_grapheme.hpp"
#include "text_breaks.hpp"
#include "text_wrap.hpp"

#include <cmath>
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
using tessera::ui::text::FontMetrics;
using tessera::ui::text::layout_text;
using tessera::ui::text::layout_text_with_breaks;
using tessera::ui::text::line_boundaries;
using tessera::ui::text::StubFontMetricsProvider;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextLayout;
using tessera::ui::text::TextLine;
using tessera::ui::text::TextStyle;
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
// for letters, digits, underscores, and whitespace runs. The last two strings are the corpora the
// existing wrap/paint fixtures use (tests/unit/paint_text_wrap_tests.cpp), so the parity matrix is
// checked against the text the current wrap output is pinned on.
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
    "one two three four five six seven eight",
    "one two three four five six seven eight nine ten",
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

    // The greedy wrap breaks after a space; UAX #14 must offer that opportunity for every interior
    // whitespace run of the fixture prose, or a wrapped line would silently overflow. Leading and
    // trailing whitespace produce no opportunity (the wrap never breaks there either).
    for (const std::string_view text : kAsciiCorpus) {
        const BreakBoundaries icu = line_boundaries(text);
        for (std::size_t index = 1; index + 1 < text.size(); ++index) {
            if (text[index] != ' ' || text[index + 1] == ' ') {
                continue;
            }
            bool found = false;
            for (const std::size_t offset : icu.offsets) {
                found = found || offset == index + 1;
            }
            expect(found, std::string("icu offers the post-space break at ") +
                              std::to_string(index + 1) + " in '" + std::string(text) + "'");
        }
    }

    // CJK: both engines break between ideographs, which is what keeps a CJK paragraph inside its
    // constraint instead of overflowing.
    const std::string_view cjk = "\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C";
    expect(same(builtin_line_boundaries(cjk),
                BreakBoundaries{{0, 3, 6, 9, 12}, BreakBackend::Builtin}),
           std::string("builtin breaks CJK per ideograph: ") + show(builtin_line_boundaries(cjk)));
    expect(same(line_boundaries(cjk), BreakBoundaries{{0, 3, 6, 9, 12}, BreakBackend::Icu}),
           std::string("icu breaks CJK per ideograph: ") + show(line_boundaries(cjk)));

    // UAX #14 class BA (em dash): ICU offers the break after the dash, the builtin rules do not.
    // This is the boundary-level reason for the wrap difference pinned in check_wrap_path_parity.
    constexpr std::string_view em_dash = "aaaa\xE2\x80\x94"
                                         "bbbb";
    bool icu_breaks_after_dash = false;
    for (const std::size_t offset : line_boundaries(em_dash).offsets) {
        icu_breaks_after_dash = icu_breaks_after_dash || offset == 7;
    }
    expect(icu_breaks_after_dash, "icu breaks after an em dash");
    expect(builtin_line_boundaries(em_dash).offsets.size() == 2,
           std::string("builtin has no break after an em dash: ") +
               show(builtin_line_boundaries(em_dash)));
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

TextStyle wrap_style() {
    TextStyle style;
    style.font_size = 16.0f;
    style.line_height = 20.0f;
    return style;
}

TextLayout wrap(std::string_view text, float max_width, BreakBackend backend) {
    TextConstraints constraints;
    constraints.wrap = true;
    constraints.max_width = max_width;
    return layout_text_with_breaks(text, wrap_style(), FontMetrics{12.8f, 3.2f, 3.2f}, constraints,
                                   backend);
}

std::string show_lines(const TextLayout& layout) {
    std::string out = "[";
    for (const TextLine& line : layout.lines) {
        out += "(" + std::to_string(line.begin) + "," + std::to_string(line.end) + ")@" +
               std::to_string(static_cast<int>(line.width)) + " ";
    }
    out += "]";
    return out;
}

void expect_same_wrap(std::string_view text, float max_width, const TextLayout& baseline,
                      const TextLayout& candidate, std::string_view what) {
    bool same = baseline.lines.size() == candidate.lines.size();
    for (std::size_t index = 0; same && index < baseline.lines.size(); ++index) {
        same = baseline.lines[index].begin == candidate.lines[index].begin &&
               baseline.lines[index].end == candidate.lines[index].end &&
               std::abs(baseline.lines[index].width - candidate.lines[index].width) <= 0.001f;
    }
    if (same) {
        return;
    }
    std::cerr << "text_breaks_parity: " << what << " wrap mismatch on '" << text << "' at "
              << static_cast<int>(max_width) << " px: " << show_lines(baseline) << " vs "
              << show_lines(candidate) << "\n";
    ++failures;
}

// The whole wrap path, not just the boundary sets: on the single-spaced corpus the two engines
// must produce identical line boxes, so the builtin engine stays the baseline the wrap and paint
// fixtures are pinned on, and the ICU wrap cannot drift silently.
void check_wrap_path_parity() {
    for (const std::string_view text : kAsciiCorpus) {
        for (const float max_width : {40.0f, 80.0f, 120.0f}) {
            expect_same_wrap(text, max_width, wrap(text, max_width, BreakBackend::Builtin),
                             wrap(text, max_width, BreakBackend::Icu), "ascii");
        }
    }

    // The public entry point is the ICU engine in this build.
    const TextStyle style = wrap_style();
    const FontMetrics metrics = StubFontMetricsProvider{}.metrics(style);
    for (const std::string_view text : kAsciiCorpus) {
        TextConstraints constraints;
        constraints.max_width = 80.0f;
        expect_same_wrap(text, 80.0f, layout_text(text, style, metrics, constraints),
                         wrap(text, 80.0f, BreakBackend::Icu), "active");
    }

    // Documented wrap-path difference: UAX #14 class BA gives ICU an opportunity after an em dash
    // that the builtin reduced rules do not have, so the builtin engine splits that run at the
    // measured byte budget instead of at the dash.
    constexpr std::string_view em_dash = "aaaa\xE2\x80\x94"
                                         "bbbb";
    const TextLayout builtin = wrap(em_dash, 56.0f, BreakBackend::Builtin);
    const TextLayout icu = wrap(em_dash, 56.0f, BreakBackend::Icu);
    expect(builtin.lines.size() == 2 && builtin.lines[0].begin == 0 && builtin.lines[0].end == 9 &&
               std::abs(builtin.lines[0].width - 56.0f) <= 0.001f,
           std::string("builtin splits the em dash run at the byte budget: ") +
               show_lines(builtin));
    expect(icu.lines.size() == 2 && icu.lines[0].begin == 0 && icu.lines[0].end == 7 &&
               std::abs(icu.lines[0].width - 40.0f) <= 0.001f,
           std::string("icu breaks after the em dash: ") + show_lines(icu));

    // Two runs over the same input are byte-identical (docs/design/text.md section 6).
    expect_same_wrap(em_dash, 56.0f, wrap(em_dash, 56.0f, BreakBackend::Icu),
                     wrap(em_dash, 56.0f, BreakBackend::Icu), "repeat");
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
    check_wrap_path_parity();
    check_determinism();

    if (failures != 0) {
        std::cerr << "text_breaks_parity: " << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "text_breaks_parity: ok\n";
    return 0;
}
