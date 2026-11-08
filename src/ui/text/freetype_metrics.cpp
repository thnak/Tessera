// SPDX-License-Identifier: MIT

#include "tessera/ui/text/freetype_metrics.hpp"

#include <algorithm>
#include <utility>

namespace tessera::ui::text {

FreeTypeFontMetricsProvider::FreeTypeFontMetricsProvider(FontFace face)
    : face_(std::move(face)) {}

FontMetrics FreeTypeFontMetricsProvider::metrics(const TextStyle& style) const {
    const FontFaceMetrics design = face_.design_metrics();
    if (design.units_per_em == 0 || style.font_size <= 0.0f) {
        return {};
    }

    const float scale = style.font_size / static_cast<float>(design.units_per_em);
    FontMetrics result;
    result.ascent = static_cast<float>(design.ascender) * scale;
    result.descent = -static_cast<float>(design.descender) * scale;
    // FreeType's height is the recommended line advance, including the line gap.
    const float natural_height = static_cast<float>(design.height) * scale;
    result.line_gap = std::max(0.0f, natural_height - result.ascent - result.descent);
    return result;
}

} // namespace tessera::ui::text
