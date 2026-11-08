// SPDX-License-Identifier: MIT

#include "tessera/ui/text/text_layout.hpp"

#include <cmath>
#include <iostream>

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

int main() {
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
