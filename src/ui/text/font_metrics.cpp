// SPDX-License-Identifier: MIT

#include "tessera/ui/text/font_metrics.hpp"

namespace tessera::ui::text {

FontMetrics StubFontMetricsProvider::metrics(const TextStyle& style) const {
    const float size = style.font_size;
    return FontMetrics{size * 0.8f, size * 0.2f, size * 0.2f};
}

} // namespace tessera::ui::text
