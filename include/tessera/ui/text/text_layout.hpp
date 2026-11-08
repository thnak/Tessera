// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/font_metrics.hpp"

#include <string_view>
#include <vector>

namespace tessera::ui::text {

struct TextLine {
    float width = 0.0f;
    float height = 0.0f;
    float baseline = 0.0f;
};

struct TextLayout {
    std::vector<TextLine> lines;
    float width = 0.0f;
    float height = 0.0f;
    float baseline = 0.0f;
};

TextLayout layout_text(std::string_view text, const TextStyle& style, const FontMetrics& metrics,
                       const TextConstraints& constraints);

} // namespace tessera::ui::text
