// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/paint/paint_text_metrics.hpp"
#include "tessera/ui/scene/paint_data.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <string_view>

namespace tessera::ui {

// A `Text` leaf with `display: flex` has its text as the one anonymous flex item, which a browser
// centres with `justify-content` / `align-items` (`<span style="display:flex;align-items:center;
// justify-content:center">1</span>`, the step badge of the showcase). The node has no child box for
// layout to place, so paint applies the same two axes to its own line box.
struct FlexTextCentering {
    bool horizontal = false;
    bool vertical = false;
};

// Paint's wrap decision for a text node: `paint_wraps` and a wrapping `white-space`, except that a
// default-wrapping `Text` whose box holds only one line paints on one line. Layout decided the
// line count; an exact-fit label ("Primary" in its own measured width) must not wrap in paint when
// the two metric paths round a hair apart.
[[nodiscard]] bool text_wraps(std::string_view element_kind, const PaintData& data,
                              text::WhiteSpace white_space,
                              const paint::PaintTextMetricsSource& metrics,
                              const text::TextStyle& style, float content_height);

[[nodiscard]] FlexTextCentering flex_text_centering(std::string_view element_kind,
                                                    const style::ComputedStyle& style);

} // namespace tessera::ui
