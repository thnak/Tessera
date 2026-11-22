// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/detail/text_util.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/style/cascade.hpp"

#include <string>
#include <string_view>

#include <yoga/Yoga.h>

namespace tessera::ui::layout::detail {

// Lower-cases ASCII and trims whitespace of CSS keyword/number tokens.
[[nodiscard]] inline std::string lower(std::string_view value) {
    return ::tessera::detail::lower(value);
}
[[nodiscard]] inline std::string trim(std::string_view value) {
    return std::string(::tessera::detail::trim(value));
}

// True when `value` is non-null and equals `keyword` after trim + lower-casing. Allocation-free:
// `apply_style` asks this ~32 times per node, and the `lower(trim(...))` form it used to take
// allocated two `std::string`s per call - 64 allocations per node, per layout pass (issue #735).
[[nodiscard]] bool is_keyword(const std::string* value, std::string_view keyword);

// The same comparison for a value already in hand, without the null check.
[[nodiscard]] bool equals_keyword(std::string_view value, std::string_view keyword) noexcept;

// Parses a finite float token; false (value untouched) on anything else.
[[nodiscard]] bool parse_number(std::string_view text, float& value);

// `viewport` with `font_size` set to the element's own computed font size (px), the meaning of `em`
// in its layout lengths (issue #608).
[[nodiscard]] LayoutViewport with_element_font_size(const LayoutViewport& viewport,
                                                    const style::ComputedStyle& style);

// Maps the computed CSS declarations that Yoga understands onto `node` (display, flex, sizing,
// margin/padding/border, position, alignment, gap, ...). Unknown or invalid values are ignored.
void apply_style(YGNodeRef node, const style::ComputedStyle& style, const LayoutViewport& viewport);

// A block-level child with `width: auto` and an auto side margin (`margin: 0 auto`) fills its
// container, capped by `max-width`, and the auto margins take the rest, as in CSS. Yoga would
// shrink it to its content, so the fill is made explicit. Call after `apply_style`, for a child of
// a `display: block` parent, or for the root (which has the viewport as its container) (issue
// #611).
void apply_block_auto_margin_fill(YGNodeRef node, bool is_root);

// Rules for a child of a `display: block` parent, after `apply_style`: the auto-margin fill above,
// and no shrinking. A block child is not a flex item, so a short container never squeezes it (it
// overflows, as in a browser); the initial `flex-shrink: 1` applies only to real flex items.
void apply_block_child_style(YGNodeRef node);

// The root is at least the viewport tall and grows with its content (a document, not a fixed
// window): height auto, `min-height` the available height, unless the author set either.
void apply_root_min_height(YGNodeRef root, float available_height);

// Where a laid-out root starts horizontally: Yoga leaves a root's auto margins at zero, so the
// leftover width is split by hand (both auto: half, left only: all of it).
[[nodiscard]] float root_auto_margin_offset(YGNodeRef root, float available_width);

} // namespace tessera::ui::layout::detail
