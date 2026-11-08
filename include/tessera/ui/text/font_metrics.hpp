// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/text_measure.hpp"

#include <memory>

namespace tessera::ui::text {

struct FontMetrics {
    float ascent = 0.0f;
    float descent = 0.0f;
    float line_gap = 0.0f;
};

class FontMetricsProvider {
public:
    virtual ~FontMetricsProvider() = default;

    [[nodiscard]] virtual FontMetrics metrics(const TextStyle& style) const = 0;
};

class StubFontMetricsProvider final : public FontMetricsProvider {
public:
    [[nodiscard]] FontMetrics metrics(const TextStyle& style) const override;
};

} // namespace tessera::ui::text
