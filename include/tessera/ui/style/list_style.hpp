// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Value grammar of `list-style-type` and `list-style-position` (docs/design/list-style.md). One
// definition shared by the registry validator, `@supports`, the `list-style` shorthand and the
// marker painter, so a value the stylesheet accepts is a value the paint pass can draw.
namespace tessera::ui::style {

enum class ListStyleType : std::uint8_t {
    None,
    Disc,
    Circle,
    Square,
    Decimal,
    LowerAlpha,
    UpperAlpha,
    LowerRoman,
    UpperRoman,
};

// `none | disc | circle | square | decimal | lower-alpha | lower-latin | upper-alpha | upper-latin
// | lower-roman | upper-roman`. Every other counter style (`georgian`, `cjk-*`, a quoted string,
// `symbols()`) is not drawable and returns nullopt, so the declaration is dropped with a
// diagnostic.
[[nodiscard]] std::optional<ListStyleType> parse_list_style_type(std::string_view value);

// V1 places the marker outside the item's box, as the UA sheet of every browser does for `ul`/`ol`.
// `inside` would flow the marker into the item's first line, which the layout does not model, so it
// is rejected rather than approximated.
[[nodiscard]] bool is_supported_list_style_position(std::string_view value);

// True for the three shape markers (a disc, a ring or a square drawn as geometry, not text).
[[nodiscard]] bool is_bullet_marker(ListStyleType type);

// The counter text of an ordered marker including its suffix, for example `1. `, `b. ` or `iv. `.
// `ordinal` is 1-based. A value the style cannot spell (alphabetic or roman 0, roman above 3999)
// falls back to decimal, as CSS does. Empty for `none` and the bullet markers.
[[nodiscard]] std::string list_marker_text(ListStyleType type, std::uint32_t ordinal);

} // namespace tessera::ui::style
