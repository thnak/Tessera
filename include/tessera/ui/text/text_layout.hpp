// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/text/font_metrics.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace tessera::ui::text {

struct TextLine {
    float width = 0.0f;
    float height = 0.0f;
    float baseline = 0.0f;
    // Byte range of this line in the original UTF-8 text (hard newlines and inter-word separators
    // are excluded from both ends; a line's slice may contain the original inner separators).
    std::size_t begin = 0;
    std::size_t end = 0;
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
