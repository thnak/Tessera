// SPDX-License-Identifier: MIT
//
// Bidi layout contract: paragraph direction (first-strong), per-line visual runs from SheenBidi,
// LTR byte-identical non-regression, line splits across direction changes, and logical-to-visual
// caret mapping with the documented right-biased run-join affinity. Structural only: the
// deterministic fixture font has no Hebrew/Arabic glyphs, so pixel proof is deferred (documented).

#include "tessera/ui/text/bidi.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using tessera::ui::text::bidi_apply_visual_runs;
using tessera::ui::text::bidi_offset_x;
using tessera::ui::text::bidi_paragraph_direction;
using tessera::ui::text::BidiVisualRun;
using tessera::ui::text::layout_text;
using tessera::ui::text::StubFontMetricsProvider;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextDirection;
using tessera::ui::text::TextLayout;
using tessera::ui::text::TextLine;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_bidi: " << message << "\n";
    }
    return condition;
}

TextStyle style() {
    TextStyle result;
    result.font_size = 16.0f;
    return result;
}

TextLayout layout(std::string_view text, float width = 1000.0f, bool wrap = true) {
    TextConstraints constraints;
    constraints.wrap = wrap;
    constraints.max_width = width;
    const TextStyle text_style = style();
    return layout_text(text, text_style, StubFontMetricsProvider{}.metrics(text_style),
                       constraints);
}

float advance(std::string_view text, const TextStyle& /*text_style*/, void* /*user*/) {
    return layout(text, 100000.0f, false).width;
}

bool runs_cover_line(const TextLine& line) {
    if (line.visual_runs.empty()) {
        return false;
    }
    std::vector<BidiVisualRun> sorted = line.visual_runs;
    std::sort(sorted.begin(), sorted.end(), [](const BidiVisualRun& lhs, const BidiVisualRun& rhs) {
        return lhs.begin < rhs.begin;
    });
    std::size_t cursor = line.begin;
    for (const BidiVisualRun& run : sorted) {
        if (run.begin != cursor || run.end <= run.begin || run.end > line.end) {
            return false;
        }
        cursor = run.end;
    }
    return cursor == line.end;
}

bool test_paragraph_direction() {
    return expect(bidi_paragraph_direction("Hello") == TextDirection::LeftToRight,
                  "Latin text defaults to LTR") &&
           expect(bidi_paragraph_direction("123 456") == TextDirection::LeftToRight,
                  "digits-only text defaults to LTR") &&
           expect(bidi_paragraph_direction("\xD7\x90\xD7\x91\xD7\x92") ==
                      TextDirection::RightToLeft,
                  "Hebrew first strong character is RTL") &&
           expect(bidi_paragraph_direction("\xD8\xA7\xD8\xA8\xD8\xAC") ==
                      TextDirection::RightToLeft,
                  "Arabic first strong character is RTL") &&
           expect(bidi_paragraph_direction("abc \xD7\x90\xD7\x91") == TextDirection::LeftToRight,
                  "Latin first strong character wins") &&
           expect(bidi_paragraph_direction("\xD7\x90\xD7\x91 abc") == TextDirection::RightToLeft,
                  "Hebrew first strong character wins");
}

bool test_ltr_non_regression() {
    const std::string text = "The quick brown fox jumps";
    TextConstraints constraints;
    constraints.wrap = true;
    constraints.max_width = 80.0f;
    const TextStyle text_style = style();
    const TextLayout result =
        layout_text(text, text_style, StubFontMetricsProvider{}.metrics(text_style), constraints);
    bool ok = expect(result.direction == TextDirection::LeftToRight, "LTR direction") &&
              expect(!result.lines.empty(), "LTR lines exist");
    for (const TextLine& line : result.lines) {
        ok = expect(line.visual_runs.empty(),
                    "pure LTR lines keep the logical path (no visual runs)") &&
             ok;
        // Stub layout: one advance per code point (8 px at font size 16).
        const std::size_t codepoints = line.end - line.begin;
        ok = expect(std::abs(line.width - static_cast<float>(codepoints) * 8.0f) < 0.01f,
                    "LTR line widths are unchanged by bidi integration") &&
             ok;
    }
    return ok;
}

bool test_pure_rtl_and_numbers() {
    const std::string hebrew = "\xD7\x90\xD7\x91\xD7\x92\xD7\x93";
    const TextLayout rtl = layout(hebrew);
    bool ok = expect(rtl.direction == TextDirection::RightToLeft, "Hebrew paragraph is RTL") &&
              expect(rtl.lines.size() == 1, "Hebrew paragraph has one line");
    if (!rtl.lines.empty()) {
        const TextLine& line = rtl.lines.front();
        ok = expect(line.visual_runs.size() == 1 && (line.visual_runs[0].level & 1U) != 0U,
                    "pure RTL line is one odd-level visual run") &&
             ok;
        ok = expect(runs_cover_line(line), "RTL visual run covers the line range") && ok;
        const float start_x = bidi_offset_x(hebrew, line, line.begin, style(), &advance, nullptr);
        const float end_x = bidi_offset_x(hebrew, line, line.end, style(), &advance, nullptr);
        ok = expect(std::abs(start_x - line.width) < 0.01f && std::abs(end_x) < 0.01f,
                    "RTL caret maps logically start->right edge, end->left edge") &&
             ok;
    }

    const std::string numbers = "\xD7\x90\xD7\x91\xD7\x92 123";
    const TextLayout mixed_numbers = layout(numbers);
    ok = expect(mixed_numbers.lines.size() == 1 && !mixed_numbers.lines.front().visual_runs.empty(),
                "numbers inside RTL text get visual runs") &&
         ok;
    if (!mixed_numbers.lines.empty()) {
        bool has_even_number_run = false;
        for (const BidiVisualRun& run : mixed_numbers.lines.front().visual_runs) {
            has_even_number_run = has_even_number_run || (run.level & 1U) == 0U;
        }
        ok = expect(has_even_number_run, "RTL numbers resolve to an even level run") && ok;
        ok = expect(runs_cover_line(mixed_numbers.lines.front()),
                    "mixed number visual runs cover the line") &&
             ok;
    }
    return ok;
}

bool test_mixed_direction_and_join_affinity() {
    const std::string text = "abc \xD7\x90\xD7\x91\xD7\x92";
    const TextLayout mixed = layout(text);
    bool ok = expect(mixed.direction == TextDirection::LeftToRight,
                     "mixed paragraph with Latin first is LTR") &&
              expect(mixed.lines.size() == 1, "mixed paragraph is one line");
    if (mixed.lines.empty()) {
        return false;
    }
    const TextLine& line = mixed.lines.front();
    ok = expect(line.visual_runs.size() == 2, "mixed line has two visual runs") && ok;
    ok = expect(runs_cover_line(line), "mixed visual runs cover the line") && ok;
    if (line.visual_runs.size() == 2) {
        ok = expect(line.visual_runs[0].begin == line.begin &&
                        line.visual_runs[0].end == line.begin + 4,
                    "LTR run is first in visual order") &&
             ok;
        ok = expect(line.visual_runs[1].begin == line.begin + 4,
                    "RTL run follows in visual order") &&
             ok;
        // Right-biased affinity at the join: offset at the LTR run end sits at that run's end.
        const float join_x = bidi_offset_x(text, line, line.begin + 4, style(), &advance, nullptr);
        ok = expect(std::abs(join_x - 32.0f) < 0.01f,
                    "caret at a run join uses the preceding logical run's visual end") &&
             ok;
    }
    const TextLayout repeat = layout(text);
    ok = expect(repeat.lines.size() == mixed.lines.size() &&
                    repeat.lines.front().visual_runs == line.visual_runs,
                "bidi visual runs are deterministic") &&
         ok;
    return ok;
}

bool test_lines_across_direction_changes() {
    const std::string text = "abc \xD7\x90\xD7\x91\xD7\x92 def \xD7\x93\xD7\x94\xD7\x95 ghi jkl";
    const TextLayout layout_wrapped = layout(text, 72.0f);
    bool ok = expect(layout_wrapped.lines.size() > 1, "narrow mixed text wraps") &&
              expect(layout_wrapped.direction == TextDirection::LeftToRight,
                     "wrapped paragraph keeps its paragraph direction");
    for (const TextLine& line : layout_wrapped.lines) {
        if (!line.visual_runs.empty()) {
            ok = expect(runs_cover_line(line), "each wrapped bidi line's runs cover it") && ok;
        }
    }
    const TextLayout repeat = layout(text, 72.0f);
    ok =
        expect(repeat.lines.size() == layout_wrapped.lines.size(), "wrap count is deterministic") &&
        ok;
    for (std::size_t index = 0; index < repeat.lines.size() && index < layout_wrapped.lines.size();
         ++index) {
        ok = expect(repeat.lines[index].begin == layout_wrapped.lines[index].begin &&
                        repeat.lines[index].end == layout_wrapped.lines[index].end,
                    "wrap ranges are deterministic") &&
             ok;
    }
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = test_paragraph_direction() && ok;
    ok = test_ltr_non_regression() && ok;
    ok = test_pure_rtl_and_numbers() && ok;
    ok = test_mixed_direction_and_join_affinity() && ok;
    ok = test_lines_across_direction_changes() && ok;
    if (!ok) {
        std::cerr << "text_bidi: failed\n";
        return 1;
    }
    std::cout << "text_bidi: ok\n";
    return 0;
}
