// SPDX-License-Identifier: MIT

#include "layout_style.hpp"

#include <string>
#include <string_view>

namespace tessera::ui::layout::detail {

LayoutViewport with_element_font_size(const LayoutViewport& viewport,
                                      const style::ComputedStyle& style) {
    LayoutViewport scoped = viewport;
    // The cascade resolved `font-size` to px (`resolve_font_size`); anything else keeps the root.
    if (const std::string* size = style.find("font-size")) {
        const std::string text = lower(trim(*size));
        float px = 0.0f;
        if (text.size() > 2 && text.ends_with("px") &&
            parse_number(std::string_view(text).substr(0, text.size() - 2), px) && px > 0.0f) {
            scoped.font_size = px;
        }
    }
    return scoped;
}

} // namespace tessera::ui::layout::detail
