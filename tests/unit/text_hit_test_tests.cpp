// SPDX-License-Identifier: MIT
//
// Point-to-caret hit testing: LTR/RTL/mixed visual runs, line selection/clamping, grapheme
// snapping, join affinity, and the controller's place/extend selection helpers.

#include "tessera/ui/text/edit_controller.hpp"
#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/hit_test.hpp"
#include "tessera/ui/text/text_layout.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

namespace {

using tessera::ui::text::CaretAffinity;
using tessera::ui::text::EditController;
using tessera::ui::text::FontMetrics;
using tessera::ui::text::hit_test_point;
using tessera::ui::text::layout_text;
using tessera::ui::text::StubFontMetricsProvider;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextHit;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_hit_test: " << message << "\n";
    }
    return condition;
}

TextStyle style() {
    TextStyle result;
    result.font_size = 16.0f;
    return result;
}

float advance(std::string_view text, const TextStyle& text_style, void* user) {
    const auto* metrics = static_cast<const FontMetrics*>(user);
    return layout_text(text, text_style, *metrics, TextConstraints{}).width;
}

TextHit hit(std::string_view text, float x, float y, bool wrap = false, float max_width = 1000.0f) {
    const TextStyle text_style = style();
    const FontMetrics metrics = StubFontMetricsProvider{}.metrics(text_style);
    TextConstraints constraints;
    constraints.wrap = wrap;
    constraints.max_width = max_width;
    return hit_test_point(text, text_style, constraints, x, y, metrics, &advance,
                          const_cast<FontMetrics*>(&metrics));
}

bool test_ltr_lines_and_clamping() {
    return expect(hit("abc", 0.0f, 10.0f).byte_offset == 0, "LTR before first glyph") &&
           expect(hit("abc", 10.0f, 10.0f).byte_offset == 1, "LTR nearest boundary") &&
           expect(hit("abc", 100.0f, 10.0f).byte_offset == 3, "LTR past end clamps to end") &&
           expect(hit("abc", -10.0f, 10.0f).byte_offset == 0, "LTR before start clamps") &&
           expect(hit("abc\ndef", 0.0f, 1000.0f, true).byte_offset == 4,
                  "point below clamps to last line") &&
           expect(hit("abc\ndef", 0.0f, -10.0f, true).byte_offset == 0,
                  "point above clamps to first line");
}

bool test_rtl_visual_to_logical() {
    const std::string hebrew = "\xD7\x90\xD7\x91\xD7\x92"; // 3 Hebrew chars, 24 px at stub width
    return expect(hit(hebrew, 24.0f, 10.0f).byte_offset == 0,
                  "RTL logical start is the visual right edge") &&
           expect(hit(hebrew, 0.0f, 10.0f).byte_offset == 6,
                  "RTL visual left edge is the logical end") &&
           expect(hit(hebrew, 23.0f, 10.0f).byte_offset == 0,
                  "RTL near right edge snaps to the logical start");
}

bool test_mixed_join_affinity() {
    const std::string mixed = "abc \xD7\x90\xD7\x91"; // LTR "abc " then RTL "אב"
    const TextHit join = hit(mixed, 32.0f, 10.0f);
    return expect(join.byte_offset == 4, "mixed join maps to the logical run end") &&
           expect(join.affinity == CaretAffinity::Upstream,
                  "join uses the right-biased/upstream affinity") &&
           expect(hit(mixed, 0.0f, 10.0f).byte_offset == 0, "mixed left edge is logical start") &&
           expect(hit(mixed, 48.0f, 10.0f).byte_offset == 4,
                  "mixed RTL right edge is the logical run start");
}

bool test_grapheme_snapping() {
    const std::string composed = "e\xCC\x81x"; // e + combining acute + x
    const TextHit inside = hit(composed, 10.0f, 10.0f);
    return expect(inside.byte_offset == 0 || inside.byte_offset == 3,
                  "interior of a grapheme snaps to a cluster boundary") &&
           expect(inside.byte_offset != 1 && inside.byte_offset != 2,
                  "never returns a code-point interior of a cluster");
}

bool test_controller_place_and_extend() {
    EditController controller;
    controller.set_text("hello world");
    const TextHit first = hit("hello world", 0.0f, 10.0f);
    controller.place_caret_at(first);
    bool ok = expect(controller.buffer().caret() == 0, "place_caret_at moves the caret");
    const TextHit third = hit("hello world", 24.0f, 10.0f);
    controller.extend_selection_to(third);
    ok = expect(controller.buffer().has_selection() && controller.buffer().selection_start() == 0 &&
                    controller.buffer().selection_end() == 3,
                "extend_selection_to keeps the anchor") &&
         ok;
    controller.set_read_only(true);
    controller.place_caret_at(hit("hello world", 40.0f, 10.0f));
    ok = expect(controller.buffer().caret() == 5, "read-only allows caret moves") && ok;
    controller.set_text("abc");
    controller.place_caret_at(hit("abc", 10.0f, 10.0f));
    const TextHit repeat = hit("abc", 10.0f, 10.0f);
    ok =
        expect(controller.buffer().caret() == repeat.byte_offset, "hit testing is deterministic") &&
        ok;
    return ok;
}

} // namespace

int main() {
    bool ok = true;
    ok = test_ltr_lines_and_clamping() && ok;
    ok = test_rtl_visual_to_logical() && ok;
    ok = test_mixed_join_affinity() && ok;
    ok = test_grapheme_snapping() && ok;
    ok = test_controller_place_and_extend() && ok;
    if (!ok) {
        std::cerr << "text_hit_test: failed\n";
        return 1;
    }
    std::cout << "text_hit_test: ok\n";
    return 0;
}
