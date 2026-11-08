// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/cascade.hpp"

#include <limits>
#include <memory>
#include <string_view>

namespace tessera::ui::text {

class FontMetricsProvider;

struct TextStyle {
    float font_size = 16.0f;
    float line_height = 0.0f;
    float letter_spacing = 0.0f;
    int font_weight = 400;
    bool italic = false;
    bool nowrap = false;
};

struct TextConstraints {
    float max_width = std::numeric_limits<float>::infinity();
    bool wrap = true;
};

struct TextMetrics {
    float width = 0.0f;
    float height = 0.0f;
    float baseline = 0.0f;
    int line_count = 0;
};

class TextMeasurer {
public:
    virtual ~TextMeasurer() = default;

    [[nodiscard]] virtual TextMetrics measure(std::string_view text, const TextStyle& style,
                                              const TextConstraints& constraints) const = 0;
};

class StubTextMeasurer final : public TextMeasurer {
public:
    explicit StubTextMeasurer(
        std::shared_ptr<const FontMetricsProvider> metrics_provider = nullptr);

    [[nodiscard]] TextMetrics measure(std::string_view text, const TextStyle& style,
                                      const TextConstraints& constraints) const override;

private:
    std::shared_ptr<const FontMetricsProvider> metrics_provider_;
};

TextStyle text_style_from(const style::ComputedStyle& style);

} // namespace tessera::ui::text
