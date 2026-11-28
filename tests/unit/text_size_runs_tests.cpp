// SPDX-License-Identifier: MIT
//
// Metric inline-span runs (issue #1004, docs/design/inline-size-runs.md): the intern table, the
// wrap measuring a shrunk run at its own size (stub advance = font size / 2 per code point), and a
// run changing where a line breaks. Deterministic: the stub advance only.

#include "tessera/ui/text/size_runs.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <string>

namespace {

using namespace tessera::ui::text;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "text_size_runs: " << message << "\n";
        ++failures;
    }
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

TextLayout lay_out(const std::string& text, TextSizeRunsId runs, float max_width) {
    TextStyle style;
    style.font_size = 16.0f;
    style.size_runs = runs;
    TextConstraints constraints;
    constraints.max_width = max_width;
    constraints.wrap = true;
    return layout_text(text, style, StubFontMetricsProvider{}.metrics(style), constraints);
}

void test_intern_table() {
    const std::array<TextSizeRun, 2> runs{TextSizeRun{2, 4, 0.5f}, TextSizeRun{6, 8, 0.75f}};
    const TextSizeRunsId first = intern_text_size_runs(runs);
    const TextSizeRunsId again = intern_text_size_runs(runs);
    check(first.value != 0 && first == again, "equal tables share one id");
    check(resolve_text_size_runs(first).size() == 2, "the table resolves");
    check(near(text_size_run_scale(resolve_text_size_runs(first), 3), 0.5f), "scale inside a run");
    check(near(text_size_run_scale(resolve_text_size_runs(first), 5), 1.0f), "scale between runs");
    const std::array<TextSizeRun, 2> growing{TextSizeRun{0, 2, 1.5f}, TextSizeRun{3, 3, 0.5f}};
    check(intern_text_size_runs(growing).value == 0, "growing and empty runs are dropped");
    check(resolve_text_size_runs(TextSizeRunsId{0}).empty(), "id 0 has no runs");
    const std::array<SpanFontSize, 2> spans{SpanFontSize{0, 2, 16.0f}, SpanFontSize{2, 4, 8.0f}};
    const TextSizeRunsId from_spans = size_runs_from_spans(spans, 16.0f);
    check(resolve_text_size_runs(from_spans).size() == 1 &&
              near(resolve_text_size_runs(from_spans)[0].scale, 0.5f),
          "only the shrunk span becomes a run");
}

void test_width_follows_runs() {
    // "x2" + a 2-byte superscript at half size: 3 code points at 8 px + 2 at 4 px.
    const std::array<TextSizeRun, 1> runs{TextSizeRun{3, 5, 0.5f}};
    const TextSizeRunsId id = intern_text_size_runs(runs);
    const TextLayout plain = lay_out("abc12", {}, 1000.0f);
    const TextLayout shrunk = lay_out("abc12", id, 1000.0f);
    check(near(plain.width, 5 * 8.0f), "no runs: the paragraph size");
    check(near(shrunk.width, 3 * 8.0f + 2 * 4.0f), "runs: the span is measured at its own size");
    check(shrunk.lines.size() == 1 && near(shrunk.height, plain.height),
          "the line box stays the paragraph's");
}

void test_runs_change_the_break() {
    // Two words of 5 code points: 40 px each at 16 px. A 90 px box holds "hello world" only when
    // the second word is shrunk (40 + 8 gap + 20 = 68 px).
    const std::string text = "hello world";
    const TextLayout plain = lay_out(text, {}, 60.0f);
    check(plain.lines.size() == 2, "the plain paragraph wraps");
    const std::array<TextSizeRun, 1> runs{TextSizeRun{6, 11, 0.5f}};
    const TextLayout shrunk = lay_out(text, intern_text_size_runs(runs), 70.0f);
    check(shrunk.lines.size() == 1 && near(shrunk.width, 40.0f + 8.0f + 20.0f),
          "a shrunk word fits the line it would have wrapped from");
    const TextLayout narrow = lay_out(text, intern_text_size_runs(runs), 45.0f);
    check(narrow.lines.size() == 2 && near(narrow.lines[1].width, 20.0f),
          "the wrapped second line is measured at the span size");
}

} // namespace

int main() {
    test_intern_table();
    test_width_follows_runs();
    test_runs_change_the_break();
    return failures == 0 ? 0 : 1;
}
