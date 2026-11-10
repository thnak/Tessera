// SPDX-License-Identifier: MIT

#include "tessera/ui/text/text_layout.hpp"

#include <cmath>
#include <iostream>
#include <string_view>

namespace {

using tessera::ui::text::FontMetrics;
using tessera::ui::text::layout_text;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextLayout;
using tessera::ui::text::TextStyle;

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

    // Nine code points at 8 px: the run counts every space, not one separator per word boundary.
    const TextLayout interior =
        layout_text("foo   bar", style(), kMetrics, TextConstraints{200.0f, true});
    if (!expect(interior.lines.size() == 1, "a wide interior run stays on one line") ||
        !expect(near(interior.lines[0].width, 72.0f), "interior space runs advance their width") ||
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
