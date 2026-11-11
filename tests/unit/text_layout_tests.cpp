// SPDX-License-Identifier: MIT

#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::text::FontMetrics;
using tessera::ui::text::layout_text;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextLayout;
using tessera::ui::text::TextStyle;
using tessera::ui::text::WhiteSpace;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_layout: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

TextStyle style() {
    TextStyle result;
    result.font_size = 16.0f;
    result.line_height = 20.0f;
    return result;
}

const FontMetrics kMetrics{12.8f, 3.2f, 3.2f};

} // namespace

bool blank_lines_own_a_line_box() {
    const TextLayout after_enter =
        layout_text("a\n", style(), kMetrics, TextConstraints{200.0f, true});
    if (!expect(after_enter.lines.size() == 2, "a trailing newline adds an empty last line") ||
        !expect(after_enter.lines[1].begin == 2 && after_enter.lines[1].end == 2,
                "the empty last line sits at the end of the text")) {
        return false;
    }
    const TextLayout blank =
        layout_text("a\n\nb", style(), kMetrics, TextConstraints{200.0f, true});
    return expect(blank.lines.size() == 3, "a blank paragraph keeps its own line") &&
           expect(blank.lines[1].begin == 2 && blank.lines[1].end == 2 &&
                      near(blank.lines[1].width, 0.0f),
                  "the blank line is empty and between the neighbours") &&
           expect(near(blank.height, 60.0f), "the blank line contributes its height");
}

// The greedy wrap places UAX #14 segments (task-106): separators belong to the segment before
// them, the wrapping engine trims both edges of every segment, and interior runs keep their real
// width. All of these hold for both break engines, so this file stays backend-agnostic.
bool separators_are_trimmed_and_measured() {
    const TextLayout trailing =
        layout_text("hello world   ", style(), kMetrics, TextConstraints{200.0f, true});
    if (!expect(trailing.lines.size() == 1, "trailing spaces add no line") ||
        !expect(near(trailing.lines[0].width, 88.0f), "trailing spaces do not advance the line") ||
        !expect(trailing.lines[0].begin == 0 && trailing.lines[0].end == 11,
                "trailing spaces stay outside the line range")) {
        return false;
    }

    const TextLayout leading =
        layout_text("   hello", style(), kMetrics, TextConstraints{200.0f, true});
    if (!expect(leading.lines.size() == 1, "leading spaces add no line") ||
        !expect(near(leading.lines[0].width, 40.0f), "leading spaces do not advance the line") ||
        !expect(leading.lines[0].begin == 3 && leading.lines[0].end == 8,
                "leading spaces are trimmed at the line start")) {
        return false;
    }

    // CSS `normal` (the default) collapses the run to one space advance: seven code points at 8 px.
    const TextLayout interior =
        layout_text("foo   bar", style(), kMetrics, TextConstraints{200.0f, true});
    if (!expect(interior.lines.size() == 1, "a wide interior run stays on one line") ||
        !expect(near(interior.lines[0].width, 56.0f),
                "interior space runs collapse under normal") ||
        !expect(interior.lines[0].begin == 0 && interior.lines[0].end == 9,
                "interior spaces stay inside the line range")) {
        return false;
    }

    // The separator run stays with the line it ends; the next line starts at its first content.
    const TextLayout narrow =
        layout_text("foo   bar", style(), kMetrics, TextConstraints{50.0f, true});
    if (!expect(narrow.lines.size() == 2, "the separator run is the wrap point") ||
        !expect(narrow.lines[0].begin == 0 && narrow.lines[0].end == 3 &&
                    near(narrow.lines[0].width, 24.0f),
                "the first line ends before the separator") ||
        !expect(narrow.lines[1].begin == 6 && narrow.lines[1].end == 9,
                "the second line starts after the separator")) {
        return false;
    }

    // Deterministic: a second run produces identical line boxes.
    const TextLayout repeat =
        layout_text("foo   bar", style(), kMetrics, TextConstraints{50.0f, true});
    return expect(repeat.lines.size() == narrow.lines.size(), "wrap line count is deterministic") &&
           expect(repeat.lines[1].begin == narrow.lines[1].begin &&
                      near(repeat.lines[1].width, narrow.lines[1].width),
                  "wrap line boxes are deterministic");
}

// A UAX #14 opportunity inside a run lets the wrap fill the line instead of moving the whole token,
// and the per-ideograph CJK opportunities keep a space-less paragraph inside its constraint.
bool break_opportunities_inside_runs() {
    const TextLayout hyphen =
        layout_text("abc def-ghi", style(), kMetrics, TextConstraints{64.0f, true});
    if (!expect(hyphen.lines.size() == 2, "the hyphen is a break opportunity") ||
        !expect(hyphen.lines[0].begin == 0 && hyphen.lines[0].end == 8 &&
                    near(hyphen.lines[0].width, 64.0f),
                "the first line fills up to the hyphen") ||
        !expect(hyphen.lines[1].begin == 8 && hyphen.lines[1].end == 11,
                "the remainder moves to the next line")) {
        return false;
    }

    const TextLayout cjk = layout_text("\xE4\xBD\xA0\xE5\xA5\xBD\xE4\xB8\x96\xE7\x95\x8C", style(),
                                       kMetrics, TextConstraints{20.0f, true});
    return expect(cjk.lines.size() == 2, "a CJK run wraps between ideographs") &&
           expect(near(cjk.lines[0].width, 16.0f) && near(cjk.lines[1].width, 16.0f),
                  "two ideographs per line fit the constraint");
}

// CSS white-space modes on the segment wrap (task-110, docs/design/text.md section 6). One line box
// is pinned as (source begin, source end, measured width); the painted segment sequence of a line
// is pinned separately in `text_paint_tests`.
struct ExpectedLine {
    std::size_t begin = 0;
    std::size_t end = 0;
    float width = 0.0f;
};

const char* mode_name(WhiteSpace mode) {
    switch (mode) {
    case WhiteSpace::Normal:
        return "normal";
    case WhiteSpace::NoWrap:
        return "nowrap";
    case WhiteSpace::Pre:
        return "pre";
    case WhiteSpace::PreWrap:
        return "pre-wrap";
    case WhiteSpace::BreakSpaces:
        return "break-spaces";
    }
    return "unknown";
}

TextLayout layout_mode(std::string_view text, float max_width, WhiteSpace mode) {
    TextStyle value = style();
    value.white_space = mode;
    return layout_text(text, value, kMetrics, TextConstraints{max_width, true});
}

bool expect_lines(std::string_view text, float max_width, WhiteSpace mode,
                  std::initializer_list<ExpectedLine> expected, const char* what) {
    const TextLayout layout = layout_mode(text, max_width, mode);
    if (layout.lines.size() != expected.size()) {
        std::cerr << "text_layout: " << what << " line count in " << mode_name(mode) << ": got "
                  << layout.lines.size() << " want " << expected.size() << "\n";
        return false;
    }
    bool ok = true;
    std::size_t index = 0;
    for (const ExpectedLine& box : expected) {
        const tessera::ui::text::TextLine& line = layout.lines[index];
        if (line.begin != box.begin || line.end != box.end || !near(line.width, box.width)) {
            std::cerr << "text_layout: " << what << " line " << index << " in " << mode_name(mode)
                      << ": got (" << line.begin << "," << line.end << ")@" << line.width
                      << " want (" << box.begin << "," << box.end << ")@" << box.width << "\n";
            ok = false;
        }
        ++index;
    }
    return ok;
}

bool white_space_mode_matrix() {
    // Interior runs: normal and nowrap collapse a run to one space; pre, pre-wrap and break-spaces
    // advance every preserved space (nowrap is still one edge-trimmed line).
    bool ok =
        expect_lines("foo   bar", 200.0f, WhiteSpace::Normal, {{0, 9, 56.0f}}, "interior run") &&
        expect_lines("foo   bar", 200.0f, WhiteSpace::NoWrap, {{0, 9, 56.0f}}, "interior run") &&
        expect_lines("foo   bar", 200.0f, WhiteSpace::Pre, {{0, 9, 72.0f}}, "interior run") &&
        expect_lines("foo   bar", 200.0f, WhiteSpace::PreWrap, {{0, 9, 72.0f}}, "interior run") &&
        expect_lines("foo   bar", 200.0f, WhiteSpace::BreakSpaces, {{0, 9, 72.0f}}, "interior run");
    // The collapsed run changes where the line breaks: 56 px fits "foo   bar" only when collapsing.
    ok = expect_lines("foo   bar", 56.0f, WhiteSpace::Normal, {{0, 9, 56.0f}}, "collapsed fit") &&
         expect_lines("foo   bar", 56.0f, WhiteSpace::NoWrap, {{0, 9, 56.0f}}, "collapsed fit") &&
         expect_lines("foo   bar", 56.0f, WhiteSpace::PreWrap, {{0, 3, 24.0f}, {6, 9, 24.0f}},
                      "preserved overflow") &&
         expect_lines("foo   bar", 56.0f, WhiteSpace::Pre, {{0, 9, 72.0f}}, "pre overflow") && ok;

    // Edge runs: normal, nowrap and pre-wrap never start or end a line with separators (nowrap is
    // one line, so the same rule trims its two edges); pre and break-spaces keep them.
    ok =
        expect_lines("hello world   ", 200.0f, WhiteSpace::Normal, {{0, 11, 88.0f}},
                     "trailing run") &&
        expect_lines("hello world   ", 200.0f, WhiteSpace::NoWrap, {{0, 11, 88.0f}},
                     "trailing run") &&
        expect_lines("hello world   ", 200.0f, WhiteSpace::Pre, {{0, 14, 112.0f}},
                     "trailing run") &&
        expect_lines("hello world   ", 200.0f, WhiteSpace::PreWrap, {{0, 11, 88.0f}},
                     "trailing run") &&
        expect_lines("hello world   ", 200.0f, WhiteSpace::BreakSpaces, {{0, 14, 112.0f}},
                     "trailing run") &&
        expect_lines("   hello", 200.0f, WhiteSpace::Normal, {{3, 8, 40.0f}}, "leading run") &&
        expect_lines("   hello", 200.0f, WhiteSpace::NoWrap, {{3, 8, 40.0f}}, "leading run") &&
        expect_lines("   hello", 200.0f, WhiteSpace::Pre, {{0, 8, 64.0f}}, "leading run") &&
        expect_lines("   hello", 200.0f, WhiteSpace::BreakSpaces, {{0, 8, 64.0f}}, "leading run") &&
        ok;

    // Tabs are collapsible whitespace: one advance under the collapsing modes, one per tab byte
    // otherwise.
    ok = expect_lines("a\t\tb", 200.0f, WhiteSpace::Normal, {{0, 4, 24.0f}}, "tab run") &&
         expect_lines("a\t\tb", 200.0f, WhiteSpace::NoWrap, {{0, 4, 24.0f}}, "tab run") &&
         expect_lines("a\t\tb", 200.0f, WhiteSpace::Pre, {{0, 4, 32.0f}}, "tab run") &&
         expect_lines("a\t\tb", 200.0f, WhiteSpace::PreWrap, {{0, 4, 32.0f}}, "tab run") &&
         expect_lines("a\t\tb", 200.0f, WhiteSpace::BreakSpaces, {{0, 4, 32.0f}}, "tab run") &&
         expect_lines("a\t\tb", 24.0f, WhiteSpace::Normal, {{0, 4, 24.0f}}, "collapsed tab") &&
         expect_lines("a\t\tb", 24.0f, WhiteSpace::NoWrap, {{0, 4, 24.0f}}, "collapsed tab") &&
         expect_lines("a\t\tb", 24.0f, WhiteSpace::Pre, {{0, 4, 32.0f}}, "pre tab") &&
         expect_lines("a\t\tb", 24.0f, WhiteSpace::PreWrap, {{0, 1, 8.0f}, {3, 4, 8.0f}},
                      "preserved tab") &&
         ok;

    // Blank paragraphs keep one zero-width line box in every wrapped mode; break-spaces emits the
    // spaces themselves, so a whitespace-only paragraph is not blank there.
    ok = expect_lines("\n\n", 200.0f, WhiteSpace::Normal,
                      {{0, 0, 0.0f}, {1, 1, 0.0f}, {2, 2, 0.0f}}, "blank lines") &&
         expect_lines("\n\n", 200.0f, WhiteSpace::BreakSpaces,
                      {{0, 0, 0.0f}, {1, 1, 0.0f}, {2, 2, 0.0f}}, "blank lines") &&
         expect_lines("   \n", 200.0f, WhiteSpace::Normal, {{0, 3, 0.0f}, {4, 4, 0.0f}},
                      "space paragraph") &&
         expect_lines("   \n", 200.0f, WhiteSpace::BreakSpaces, {{0, 3, 24.0f}, {4, 4, 0.0f}},
                      "space paragraph") &&
         ok;

    // CJK: ideographs break per character under both engines; the space run between scripts still
    // follows the mode (one advance under normal, three under pre-wrap and break-spaces).
    constexpr std::string_view cjk = "\xE4\xBD\xA0\xE5\xA5\xBD   \xE4\xB8\x96\xE7\x95\x8C";
    ok = expect_lines(cjk, 200.0f, WhiteSpace::Normal, {{0, 15, 40.0f}}, "cjk space run") &&
         expect_lines(cjk, 200.0f, WhiteSpace::PreWrap, {{0, 15, 56.0f}}, "cjk space run") &&
         expect_lines(cjk, 200.0f, WhiteSpace::BreakSpaces, {{0, 15, 56.0f}}, "cjk space run") &&
         ok;
    // Narrow: the ideographs fill the line and the separator run decides `break-spaces` line units.
    constexpr std::string_view cjk_space = "\xE4\xBD\xA0\xE5\xA5\xBD \xE4\xB8\x96\xE7\x95\x8C";
    ok = expect_lines(cjk_space, 20.0f, WhiteSpace::Normal, {{0, 6, 16.0f}, {7, 13, 16.0f}},
                      "cjk narrow") &&
         expect_lines(cjk, 20.0f, WhiteSpace::BreakSpaces,
                      {{0, 6, 16.0f}, {6, 8, 16.0f}, {8, 9, 8.0f}, {9, 15, 16.0f}}, "cjk narrow") &&
         ok;

    // Em dash: no mode adds a break opportunity, so whichever engine is active splits the over-wide
    // run identically in every narrowing mode. `text_breaks_parity` pins the engines' different
    // split points (builtin byte budget versus UAX #14 class BA), which keeps this file
    // backend-agnostic.
    constexpr std::string_view em_dash = "aaaa\xE2\x80\x94"
                                         "bbbb";
    const TextLayout dash_normal = layout_mode(em_dash, 56.0f, WhiteSpace::Normal);
    ok = expect(dash_normal.lines.size() == 2, "the em dash run splits into two lines") &&
         expect_lines(em_dash, 56.0f, WhiteSpace::NoWrap, {{0, 11, 72.0f}}, "em dash") && ok;
    for (const WhiteSpace mode : {WhiteSpace::PreWrap, WhiteSpace::BreakSpaces}) {
        const TextLayout other = layout_mode(em_dash, 56.0f, mode);
        ok = expect(other.lines.size() == dash_normal.lines.size(),
                    "the em dash split is mode-independent") &&
             ok;
        for (std::size_t index = 0; index < other.lines.size() && index < dash_normal.lines.size();
             ++index) {
            ok = expect(other.lines[index].begin == dash_normal.lines[index].begin &&
                            other.lines[index].end == dash_normal.lines[index].end &&
                            near(other.lines[index].width, dash_normal.lines[index].width),
                        "the em dash split is mode-independent") &&
                 ok;
        }
    }

    // Determinism: every mode repeats its line boxes exactly.
    constexpr WhiteSpace modes[] = {WhiteSpace::Normal, WhiteSpace::NoWrap, WhiteSpace::Pre,
                                    WhiteSpace::PreWrap, WhiteSpace::BreakSpaces};
    for (const WhiteSpace mode : modes) {
        const TextLayout first = layout_mode("foo   bar\n\nbaz\t qux", 60.0f, mode);
        const TextLayout second = layout_mode("foo   bar\n\nbaz\t qux", 60.0f, mode);
        ok =
            expect(first.lines.size() == second.lines.size(), "wrap line count is deterministic") &&
            ok;
        for (std::size_t index = 0; index < first.lines.size() && index < second.lines.size();
             ++index) {
            ok = expect(first.lines[index].begin == second.lines[index].begin &&
                            first.lines[index].end == second.lines[index].end &&
                            first.lines[index].width == second.lines[index].width,
                        "wrap line boxes are deterministic in every mode") &&
                 ok;
        }
    }
    return ok;
}

int main() {
    if (!blank_lines_own_a_line_box()) {
        return 10;
    }
    if (!separators_are_trimmed_and_measured()) {
        return 11;
    }
    if (!break_opportunities_inside_runs()) {
        return 12;
    }
    if (!white_space_mode_matrix()) {
        return 13;
    }
    const TextLayout wrapped =
        layout_text("hello world", style(), kMetrics, TextConstraints{50.0f, true});
    if (!expect(wrapped.lines.size() == 2, "wrapped line count") ||
        !expect(near(wrapped.lines[0].width, 40.0f), "wrapped first width") ||
        !expect(near(wrapped.lines[1].width, 40.0f), "wrapped second width") ||
        !expect(near(wrapped.width, 40.0f), "wrapped max width") ||
        !expect(near(wrapped.height, 40.0f), "wrapped height") ||
        !expect(near(wrapped.baseline, 12.8f), "wrapped baseline") ||
        !expect(near(wrapped.lines[0].baseline, 12.8f), "line baseline")) {
        return 1;
    }

    const TextLayout nowrap =
        layout_text("hello world", style(), kMetrics, TextConstraints{50.0f, false});
    if (!expect(nowrap.lines.size() == 1, "nowrap line count") ||
        !expect(near(nowrap.lines[0].width, 88.0f), "nowrap width") ||
        !expect(near(nowrap.height, 20.0f), "nowrap height")) {
        return 2;
    }

    const TextLayout broken =
        layout_text("abcdefgh", style(), kMetrics, TextConstraints{20.0f, true});
    if (!expect(broken.lines.size() == 4, "broken line count") ||
        !expect(near(broken.lines[0].width, 16.0f), "broken width") ||
        !expect(near(broken.height, 80.0f), "broken height")) {
        return 3;
    }

    const TextLayout newlines =
        layout_text("a\nb", style(), kMetrics, TextConstraints{100.0f, true});
    if (!expect(newlines.lines.size() == 2, "newline count") ||
        !expect(near(newlines.lines[0].width, 8.0f), "newline first") ||
        !expect(near(newlines.height, 40.0f), "newline height")) {
        return 4;
    }

    const TextLayout spaces = layout_text("   ", style(), kMetrics, TextConstraints{100.0f, true});
    if (!expect(spaces.lines.size() == 1, "spaces line") ||
        !expect(near(spaces.width, 0.0f), "spaces width")) {
        return 5;
    }

    const TextLayout empty = layout_text("", style(), kMetrics, TextConstraints{100.0f, true});
    if (!expect(empty.lines.empty(), "empty lines") ||
        !expect(near(empty.width, 0.0f) && near(empty.height, 0.0f), "empty size")) {
        return 6;
    }

    std::cout << "text_layout: ok\n";
    return 0;
}
