// SPDX-License-Identifier: MIT

#include "tessera/ui/text/text_measure.hpp"

#include <cmath>
#include <iostream>
#include <string>

namespace {

using tessera::ui::style::ComputedStyle;
using tessera::ui::text::StubTextMeasurer;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextMetrics;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "text_measure: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.01f;
}

TextStyle test_style() {
    TextStyle style;
    style.font_size = 16.0f;
    style.line_height = 20.0f;
    return style;
}

} // namespace

int main() {
    const StubTextMeasurer measurer;

    const TextMetrics wrapped =
        measurer.measure("hello world", test_style(), TextConstraints{50.0f, true});
    if (!expect(near(wrapped.width, 40.0f), "wrapped width") ||
        !expect(near(wrapped.height, 40.0f), "wrapped height") ||
        !expect(wrapped.line_count == 2, "wrapped lines") ||
        !expect(near(wrapped.baseline, 12.8f), "baseline")) {
        return 1;
    }

    const TextMetrics nowrap =
        measurer.measure("hello world", test_style(), TextConstraints{50.0f, false});
    if (!expect(near(nowrap.width, 88.0f), "nowrap width") ||
        !expect(near(nowrap.height, 20.0f), "nowrap height") ||
        !expect(nowrap.line_count == 1, "nowrap lines")) {
        return 2;
    }

    const TextMetrics long_word =
        measurer.measure("abcdefgh", test_style(), TextConstraints{20.0f, true});
    if (!expect(near(long_word.width, 16.0f), "long word width") ||
        !expect(near(long_word.height, 80.0f), "long word height") ||
        !expect(long_word.line_count == 4, "long word lines")) {
        return 3;
    }

    const TextMetrics newlines =
        measurer.measure("a\nb", test_style(), TextConstraints{100.0f, true});
    if (!expect(near(newlines.width, 8.0f), "newline width") ||
        !expect(near(newlines.height, 40.0f), "newline height") ||
        !expect(newlines.line_count == 2, "newline lines")) {
        return 4;
    }

    TextStyle spaced = test_style();
    spaced.letter_spacing = 2.0f;
    const TextMetrics lettered = measurer.measure("abc", spaced, TextConstraints{1000.0f, false});
    if (!expect(near(lettered.width, 30.0f), "letter spacing")) {
        return 5;
    }

    const TextMetrics empty = measurer.measure("", test_style(), TextConstraints{100.0f, true});
    if (!expect(near(empty.width, 0.0f) && near(empty.height, 0.0f) && empty.line_count == 0,
                "empty text")) {
        return 6;
    }

    ComputedStyle computed;
    computed.set("font-size", "20px");
    computed.set("line-height", "1.5");
    computed.set("letter-spacing", "1px");
    computed.set("white-space", "nowrap");
    const TextStyle parsed = tessera::ui::text::text_style_from(computed);
    if (!expect(near(parsed.font_size, 20.0f), "font-size parse") ||
        !expect(near(parsed.line_height, 30.0f), "line-height parse") ||
        !expect(near(parsed.letter_spacing, 1.0f), "letter-spacing parse") ||
        !expect(parsed.white_space == tessera::ui::text::WhiteSpace::NoWrap, "white-space parse")) {
        return 7;
    }
    computed.set("white-space", "pre");
    if (!expect(tessera::ui::text::text_style_from(computed).white_space ==
                    tessera::ui::text::WhiteSpace::Pre,
                "white-space:pre keeps the literal mode")) {
        return 9;
    }

    ComputedStyle multiplied;
    multiplied.set("font-size", "10px");
    multiplied.set("line-height", "5");
    const TextStyle multiplied_style = tessera::ui::text::text_style_from(multiplied);
    if (!expect(near(multiplied_style.line_height, 50.0f), "unitless line-height multiplies")) {
        return 8;
    }

    std::cout << "text_measure: ok\n";
    return 0;
}
