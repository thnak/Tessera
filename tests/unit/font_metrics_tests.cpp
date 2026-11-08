// SPDX-License-Identifier: MIT

#include "tessera/ui/text/font_metrics.hpp"

#include <cmath>
#include <iostream>
#include <memory>

namespace {

using tessera::ui::text::FontMetrics;
using tessera::ui::text::FontMetricsProvider;
using tessera::ui::text::StubFontMetricsProvider;
using tessera::ui::text::StubTextMeasurer;
using tessera::ui::text::TextConstraints;
using tessera::ui::text::TextMetrics;
using tessera::ui::text::TextStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "font_metrics: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.001f;
}

class CustomMetrics final : public FontMetricsProvider {
public:
    FontMetrics metrics(const TextStyle&) const override {
        return FontMetrics{10.0f, 2.0f, 4.0f};
    }
};

} // namespace

int main() {
    TextStyle style;
    style.font_size = 16.0f;
    TextConstraints constraints;
    constraints.max_width = 100.0f;

    const StubFontMetricsProvider defaults;
    const FontMetrics metrics = defaults.metrics(style);
    if (!expect(near(metrics.ascent, 12.8f), "default ascent") ||
        !expect(near(metrics.descent, 3.2f), "default descent") ||
        !expect(near(metrics.line_gap, 3.2f), "default line gap")) {
        return 1;
    }

    const StubTextMeasurer default_measurer;
    const TextMetrics default_metrics = default_measurer.measure("abc", style, constraints);
    if (!expect(near(default_metrics.height, 19.2f), "default line height") ||
        !expect(near(default_metrics.baseline, 12.8f), "default baseline")) {
        return 2;
    }

    const StubTextMeasurer custom_measurer(std::make_shared<CustomMetrics>());
    const TextMetrics custom_metrics = custom_measurer.measure("abc", style, constraints);
    if (!expect(near(custom_metrics.height, 16.0f), "custom line height") ||
        !expect(near(custom_metrics.baseline, 10.0f), "custom baseline")) {
        return 3;
    }

    TextStyle explicit_height = style;
    explicit_height.line_height = 30.0f;
    const TextMetrics explicit_metrics =
        custom_measurer.measure("abc", explicit_height, constraints);
    if (!expect(near(explicit_metrics.height, 30.0f), "explicit line height") ||
        !expect(near(explicit_metrics.baseline, 10.0f), "explicit baseline")) {
        return 4;
    }

    std::cout << "font_metrics: ok\n";
    return 0;
}
