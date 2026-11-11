// SPDX-License-Identifier: MIT
//
// Paint-time segment transform (docs/design/text.md section 6.2): per `white-space` mode, the
// source range of a laid-out line maps to a pinned painted segment sequence, and the sum of the
// segment advances equals the laid-out line width. Deterministic stub metrics only.

#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"
#include "tessera/ui/text/text_paint.hpp"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::text::FontMetrics;
using tessera::ui::text::layout_text;
using tessera::ui::text::PaintedSegment;
using tessera::ui::text::PaintedSegmentCursor;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextLayout;
using tessera::ui::text::TextLine;
using tessera::ui::text::TextStyle;
using tessera::ui::text::WhiteSpace;

const FontMetrics kMetrics{12.8f, 3.2f, 3.2f};

TextStyle style_for(WhiteSpace mode) {
    TextStyle style;
    style.font_size = 16.0f;
    style.line_height = 20.0f;
    style.white_space = mode;
    return style;
}

// Exactly the deterministic stub advance the layout uses (font-size / 2 + letter spacing per code
// point), so segment advances are comparable with the measured line widths.
float stub_advance(std::string_view text, const TextStyle& style) {
    std::size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & 0xC0u) != 0x80u) {
            ++count;
        }
    }
    return static_cast<float>(count) * (style.font_size * 0.5f + style.letter_spacing);
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "text_paint: " << message << "\n";
    }
    return condition;
}

TextLayout layout_for(std::string_view text, WhiteSpace mode, float max_width) {
    return layout_text(text, style_for(mode), kMetrics, TextConstraints{max_width, true});
}

struct PinnedSegment {
    std::string_view text;
    std::size_t begin = 0;
    std::size_t end = 0;
    bool collapsed = false;
};

// Pins one line box and its painted segment sequence, and checks that the segments advance exactly
// the laid-out line width.
bool expect_line(std::string_view text, WhiteSpace mode, float max_width, std::size_t index,
                 std::size_t want_begin, std::size_t want_end, float want_width,
                 std::initializer_list<PinnedSegment> want, const std::string& what) {
    const TextStyle style = style_for(mode);
    const TextLayout layout = layout_for(text, mode, max_width);
    if (index >= layout.lines.size()) {
        return expect(false, what + ": missing line " + std::to_string(index));
    }
    const TextLine& line = layout.lines[index];
    bool ok =
        expect(line.begin == want_begin && line.end == want_end && near(line.width, want_width),
               what + ": line box is (" + std::to_string(line.begin) + "," +
                   std::to_string(line.end) + ")@" + std::to_string(line.width));
    PaintedSegmentCursor cursor(text, line.begin, line.end, mode, true);
    PaintedSegment segment;
    float painted = 0.0f;
    std::size_t count = 0;
    std::initializer_list<PinnedSegment>::const_iterator pinned = want.begin();
    while (cursor.next(segment)) {
        painted += stub_advance(segment.source, style);
        if (pinned != want.end()) {
            ok = expect(
                segment.source == pinned->text && segment.source_begin == pinned->begin &&
                    segment.source_end == pinned->end && segment.collapsed == pinned->collapsed,
                what + ": segment " + std::to_string(count) + " is '" +
                    std::string(segment.source) + "' [" + std::to_string(segment.source_begin) +
                    "," + std::to_string(segment.source_end) + ")" +
                    (segment.collapsed ? " collapsed" : ""));
            ++pinned;
        }
        ++count;
    }
    ok = expect(count == want.size(), what + ": painted " + std::to_string(count) +
                                          " segments, want " + std::to_string(want.size())) &&
         ok;
    return expect(near(painted, line.width), what + ": painted advance " + std::to_string(painted) +
                                                 " != line width " + std::to_string(line.width)) &&
           ok;
}

// The transform's core contract for every mode: the painted advances of a line sum to its measured
// width, for a corpus with interior runs, edge runs, tabs, hard newlines and an over-wide run.
bool advances_match_line_widths() {
    constexpr std::string_view corpus[] = {
        "foo   bar",
        "alpha  beta   ",
        "   hello",
        "a\t\tb",
        "one two three four five six seven eight",
        "line\nbreak",
        "   ",
        "\xE4\xBD\xA0\xE5\xA5\xBD   \xE4\xB8\x96\xE7\x95\x8C",
        "aaaa\xE2\x80\x94"
        "bbbb",
        "supercalifragilistic",
    };
    constexpr WhiteSpace modes[] = {WhiteSpace::Normal, WhiteSpace::NoWrap, WhiteSpace::Pre,
                                    WhiteSpace::PreWrap, WhiteSpace::BreakSpaces};
    bool ok = true;
    for (const WhiteSpace mode : modes) {
        const TextStyle style = style_for(mode);
        for (const std::string_view text : corpus) {
            for (const float max_width : {16.0f, 40.0f, 200.0f}) {
                const TextLayout layout = layout_for(text, mode, max_width);
                for (const TextLine& line : layout.lines) {
                    PaintedSegmentCursor cursor(text, line.begin, line.end, mode, true);
                    PaintedSegment segment;
                    float painted = 0.0f;
                    while (cursor.next(segment)) {
                        painted += stub_advance(segment.source, style);
                    }
                    ok = expect(near(painted, line.width),
                                "painted advance matches the line width for '" + std::string(text) +
                                    "' at " + std::to_string(static_cast<int>(max_width))) &&
                         ok;
                }
            }
        }
    }
    return ok;
}

bool pinned_normal() {
    // The reported limitation: the measured width is collapsed, so the painted slice must collapse
    // too. One interior run paints one borrowed space between the two surviving source spans.
    bool ok = expect_line("foo   bar", WhiteSpace::Normal, 200.0f, 0, 0, 9, 56.0f,
                          {{"foo", 0, 3, false}, {" ", 3, 6, true}, {"bar", 6, 9, false}},
                          "normal interior run");
    // A single plain space stays in the painted span, so ordinary prose is still one glyph run.
    ok = expect_line("hello world   ", WhiteSpace::Normal, 200.0f, 0, 0, 11, 88.0f,
                     {{"hello world", 0, 11, false}}, "normal single spaces") &&
         ok;
    // Leading and trailing runs are trimmed by the line box: nothing to paint.
    ok = expect_line("   hello", WhiteSpace::Normal, 200.0f, 0, 3, 8, 40.0f,
                     {{"hello", 3, 8, false}}, "normal leading run") &&
         ok;
    // Tabs collapse like spaces even though their source byte is not ' '.
    ok = expect_line("a\t\tb", WhiteSpace::Normal, 200.0f, 0, 0, 4, 24.0f,
                     {{"a", 0, 1, false}, {" ", 1, 3, true}, {"b", 3, 4, false}},
                     "normal tab run") &&
         ok;
    // The collapsed run decides the break: at 56 px "foo   bar" fits on one line.
    ok = expect_line("foo   bar", WhiteSpace::Normal, 56.0f, 0, 0, 9, 56.0f,
                     {{"foo", 0, 3, false}, {" ", 3, 6, true}, {"bar", 6, 9, false}},
                     "normal collapsed fit") &&
         ok;
    // A whitespace-only paragraph owns a zero-width line box: the trimmed line paints nothing.
    ok = expect_line("   ", WhiteSpace::Normal, 200.0f, 0, 0, 3, 0.0f, {}, "normal space line") &&
         ok;
    return ok;
}

bool pinned_nowrap() {
    // `nowrap` collapses like normal but stays one line, trimming both edges.
    bool ok = expect_line("foo   bar", WhiteSpace::NoWrap, 200.0f, 0, 0, 9, 56.0f,
                          {{"foo", 0, 3, false}, {" ", 3, 6, true}, {"bar", 6, 9, false}},
                          "nowrap interior run");
    ok = expect_line("hello world   ", WhiteSpace::NoWrap, 200.0f, 0, 0, 11, 88.0f,
                     {{"hello world", 0, 11, false}}, "nowrap trailing run") &&
         ok;
    ok = expect_line("   hello", WhiteSpace::NoWrap, 200.0f, 0, 3, 8, 40.0f,
                     {{"hello", 3, 8, false}}, "nowrap leading run") &&
         ok;
    // A hard newline collapses to the same one-space segment as any other collapsible run.
    ok = expect_line("a\nb", WhiteSpace::NoWrap, 200.0f, 0, 0, 3, 24.0f,
                     {{"a", 0, 1, false}, {" ", 1, 2, true}, {"b", 2, 3, false}},
                     "nowrap newline") &&
         ok;
    return ok;
}

bool pinned_preserving_modes() {
    // `pre` and `pre-wrap` keep every preserved byte, so the line paints as one run whose advance
    // is the measured literal width.
    bool ok = expect_line("foo   bar", WhiteSpace::Pre, 200.0f, 0, 0, 9, 72.0f,
                          {{"foo   bar", 0, 9, false}}, "pre interior run") &&
              expect_line("   hello", WhiteSpace::Pre, 200.0f, 0, 0, 8, 64.0f,
                          {{"   hello", 0, 8, false}}, "pre leading run") &&
              expect_line("foo   bar", WhiteSpace::PreWrap, 200.0f, 0, 0, 9, 72.0f,
                          {{"foo   bar", 0, 9, false}}, "pre-wrap interior run") &&
              expect_line("a\t\tb", WhiteSpace::PreWrap, 200.0f, 0, 0, 4, 32.0f,
                          {{"a\t\tb", 0, 4, false}}, "pre-wrap tabs") &&
              // A hard newline is a paragraph break: it owns no line and is never painted.
              expect_line("a\nb", WhiteSpace::PreWrap, 200.0f, 0, 0, 1, 8.0f, {{"a", 0, 1, false}},
                          "pre-wrap first paragraph") &&
              expect_line("a\nb", WhiteSpace::PreWrap, 200.0f, 1, 2, 3, 8.0f, {{"b", 2, 3, false}},
                          "pre-wrap second paragraph") &&
              // break-spaces keeps every preserved space and every edge.
              expect_line("alpha  beta   ", WhiteSpace::BreakSpaces, 200.0f, 0, 0, 14, 112.0f,
                          {{"alpha  beta   ", 0, 14, false}}, "break-spaces trailing run") &&
              // Every preserved space is its own line unit; the line keeps the bytes it owns.
              expect_line("a   b", WhiteSpace::BreakSpaces, 16.0f, 0, 0, 2, 16.0f,
                          {{"a ", 0, 2, false}}, "break-spaces first unit line") &&
              expect_line("a   b", WhiteSpace::BreakSpaces, 16.0f, 1, 2, 4, 16.0f,
                          {{"  ", 2, 4, false}}, "break-spaces space line") &&
              expect_line("a   b", WhiteSpace::BreakSpaces, 16.0f, 2, 4, 5, 8.0f,
                          {{"b", 4, 5, false}}, "break-spaces word line") &&
              expect_line("   ", WhiteSpace::BreakSpaces, 200.0f, 0, 0, 3, 24.0f,
                          {{"   ", 0, 3, false}}, "break-spaces space paragraph");
    return ok;
}

bool pinned_overwide() {
    // An over-wide run is split by the layout into single source spans; each line paints its own
    // bytes, so the advances still sum to the line width.
    constexpr std::string_view text = "supercalifragilistic";
    const TextLayout layout = layout_for(text, WhiteSpace::Normal, 40.0f);
    bool ok = expect(layout.lines.size() > 1, "the over-wide run wraps");
    for (std::size_t index = 0; index < layout.lines.size(); ++index) {
        const TextLine& line = layout.lines[index];
        ok = expect_line(
                 text, WhiteSpace::Normal, 40.0f, index, line.begin, line.end, line.width,
                 {{text.substr(line.begin, line.end - line.begin), line.begin, line.end, false}},
                 "over-wide line") &&
             ok;
    }
    return ok;
}

bool subrange_run_boundary() {
    // A bidi visual-run boundary can cut a separator run: only the range that owns the run's first
    // byte paints the collapsed space, so the line total is not double counted.
    constexpr std::string_view text = "a   b";
    const TextStyle style = style_for(WhiteSpace::Normal);
    bool ok = true;
    {
        PaintedSegmentCursor cursor(text, 2, 4, WhiteSpace::Normal, false);
        PaintedSegment segment;
        ok = expect(!cursor.next(segment), "a run continued from before paints nothing");
    }
    {
        PaintedSegmentCursor cursor(text, 2, 5, WhiteSpace::Normal, false);
        PaintedSegment segment;
        ok = expect(cursor.next(segment) && segment.source == "b" && segment.source_begin == 4,
                    "the range after a continued run paints its content") &&
             ok;
    }
    {
        PaintedSegmentCursor cursor(text, 0, 5, WhiteSpace::Normal, false);
        PaintedSegment segment;
        float painted = 0.0f;
        std::size_t count = 0;
        while (cursor.next(segment)) {
            painted += stub_advance(segment.source, style);
            ++count;
        }
        ok = expect(count == 3 && near(painted, 24.0f),
                    "the whole line paints one collapsed space for the run") &&
             ok;
    }
    return ok;
}

bool deterministic() {
    const TextStyle style = style_for(WhiteSpace::Normal);
    const TextLayout first = layout_for("foo   bar\nbaz\t qux", WhiteSpace::Normal, 60.0f);
    const TextLayout second = layout_for("foo   bar\nbaz\t qux", WhiteSpace::Normal, 60.0f);
    bool ok = expect(first.lines.size() == second.lines.size(), "line count is deterministic");
    for (std::size_t index = 0; index < first.lines.size() && index < second.lines.size();
         ++index) {
        PaintedSegmentCursor lhs("foo   bar\nbaz\t qux", first.lines[index].begin,
                                 first.lines[index].end, WhiteSpace::Normal, true);
        PaintedSegmentCursor rhs("foo   bar\nbaz\t qux", second.lines[index].begin,
                                 second.lines[index].end, WhiteSpace::Normal, true);
        PaintedSegment left;
        PaintedSegment right;
        bool more = true;
        while (more) {
            const bool lhs_has = lhs.next(left);
            const bool rhs_has = rhs.next(right);
            more = lhs_has && rhs_has;
            ok = expect(lhs_has == rhs_has &&
                            (!lhs_has || (left.source == right.source &&
                                          left.source_begin == right.source_begin &&
                                          left.source_end == right.source_end &&
                                          near(stub_advance(left.source, style),
                                               stub_advance(right.source, style)))),
                        "painted segments are deterministic") &&
                 ok;
        }
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = advances_match_line_widths() && ok;
    ok = pinned_normal() && ok;
    ok = pinned_nowrap() && ok;
    ok = pinned_preserving_modes() && ok;
    ok = pinned_overwide() && ok;
    ok = subrange_run_boundary() && ok;
    ok = deterministic() && ok;

    if (!ok) {
        std::cerr << "text_paint: failed\n";
        return 1;
    }
    std::cout << "text_paint: ok\n";
    return 0;
}
