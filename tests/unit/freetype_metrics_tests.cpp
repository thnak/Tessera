// SPDX-License-Identifier: MIT

#include "tessera/ui/text/font_metrics.hpp"
#include "tessera/ui/text/freetype_metrics.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <cmath>
#include <iostream>
#include <memory>

namespace {

using tessera::ui::text::FontFace;
using tessera::ui::text::FontFaceMetrics;
using tessera::ui::text::FontMetrics;
using tessera::ui::text::FreeTypeFontMetricsProvider;
using tessera::ui::text::StubTextMeasurer;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextMetrics;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "freetype_metrics: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.0001f;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "freetype_metrics: missing font path" << "\n";
        return 1;
    }
    const auto loaded = FontFace::load_from_file(argv[1]);
    if (!loaded) {
        std::cerr << "freetype_metrics: cannot load fixture: " << loaded.error().message << "\n";
        return 1;
    }

    const FontFaceMetrics design = loaded->design_metrics();
    const FreeTypeFontMetricsProvider provider(*loaded);

    TextStyle style;
    style.font_size = 16.0f;
    const FontMetrics metrics = provider.metrics(style);
    const float scale = style.font_size / static_cast<float>(design.units_per_em);

    if (!expect(near(metrics.ascent, static_cast<float>(design.ascender) * scale), "ascent") ||
        !expect(near(metrics.descent, -static_cast<float>(design.descender) * scale), "descent") ||
        !expect(near(metrics.ascent + metrics.descent + metrics.line_gap,
                     static_cast<float>(design.height) * scale),
                "natural line height") ||
        !expect(metrics.line_gap >= 0.0f, "non-negative line gap")) {
        return 2;
    }

    const FontMetrics repeated = provider.metrics(style);
    if (!expect(near(repeated.ascent, metrics.ascent) && near(repeated.descent, metrics.descent) &&
                    near(repeated.line_gap, metrics.line_gap),
                "metrics are deterministic")) {
        return 3;
    }

    TextStyle larger = style;
    larger.font_size = 32.0f;
    const FontMetrics doubled = provider.metrics(larger);
    if (!expect(near(doubled.ascent, metrics.ascent * 2.0f), "scaled ascent") ||
        !expect(near(doubled.descent, metrics.descent * 2.0f), "scaled descent") ||
        !expect(near(doubled.line_gap, metrics.line_gap * 2.0f), "scaled line gap")) {
        return 4;
    }

    TextStyle zero = style;
    zero.font_size = 0.0f;
    const FontMetrics empty = provider.metrics(zero);
    if (!expect(empty.ascent == 0.0f && empty.descent == 0.0f && empty.line_gap == 0.0f,
                "zero size yields zero metrics")) {
        return 5;
    }

    const FreeTypeFontMetricsProvider invalid{FontFace{}};
    const FontMetrics none = invalid.metrics(style);
    if (!expect(none.ascent == 0.0f && none.descent == 0.0f && none.line_gap == 0.0f,
                "invalid face yields zero metrics")) {
        return 6;
    }

    const StubTextMeasurer measurer(std::make_shared<FreeTypeFontMetricsProvider>(*loaded));
    TextConstraints constraints;
    constraints.wrap = false;
    const TextMetrics text = measurer.measure("abc", style, constraints);
    if (!expect(near(text.baseline, metrics.ascent), "baseline follows font ascent") ||
        !expect(near(text.height, metrics.ascent + metrics.descent + metrics.line_gap),
                "line box follows font metrics") ||
        !expect(text.line_count == 1, "single line")) {
        return 7;
    }

    std::cout << "freetype_metrics: ok\n";
    return 0;
}
