// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::compiler::analysis {

// A class selector (`.card`) of a style sheet: the name without the dot and its UTF-16 range.
struct CssClassSite {
    std::string name;
    std::uint32_t from = 0;
    std::uint32_t to = 0;
};

// The class selectors of a CSS file in source order. Only selector preludes count (not
// declaration values such as `url(a.png)` or `1.5em`), comments and strings are skipped, and the
// body of an at-rule group (`@media`) is scanned like the top level. The index does not parse CSS
// (spec section 10), so this is the one place that reads class names out of a style sheet.
[[nodiscard]] std::vector<CssClassSite> scan_css_classes(std::string_view css);

} // namespace tessera::compiler::analysis
