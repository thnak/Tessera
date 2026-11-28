// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

// The font-variant-* keyword grammar (CSS Fonts 4 section 6, issue #998; docs/design/text.md
// section 3.1). One table owns every keyword, its group and the OpenType features it selects, so
// registry validation, the `font-variant` shorthand and the shaper's feature list cannot drift.
// The text layer reads the feature settings; the style layer only judges the grammar.
namespace tessera::ui::style {

enum class FontVariantGroup : std::uint8_t { Ligatures, Numeric, Caps, EastAsian, Position };

// One OpenType feature a keyword turns on or off.
struct FontVariantSetting {
    char tag[5];
    std::uint8_t value;
};

struct FontVariantKeyword {
    std::string_view name;
    FontVariantGroup group;
    // Keywords of one group sharing a non-zero exclusive class conflict (`jis78 jis83`,
    // `small-caps petite-caps`); a class repeated in one value is invalid. 255 means the keyword
    // must stand alone (`none`).
    std::uint8_t exclusive;
    std::uint8_t count;
    std::array<FontVariantSetting, 5> settings;
};

inline constexpr std::size_t kMaxFontVariantWords = 8;

struct FontVariantParsed {
    std::array<const FontVariantKeyword*, kMaxFontVariantWords> items{};
    std::uint8_t count = 0;
};

// Parses a whole longhand value (case-insensitive, space separated). `normal` is valid and yields
// no keywords. Empty on an unknown keyword, a keyword of another group, a repeated exclusive class
// or more than kMaxFontVariantWords words.
[[nodiscard]] std::optional<FontVariantParsed> parse_font_variant_value(FontVariantGroup group,
                                                                        std::string_view value);

// The group a keyword belongs to, or empty for an unknown word (used by the shorthand).
[[nodiscard]] const FontVariantKeyword* find_font_variant_keyword(std::string_view lowered);

// The longhand property name of a group, e.g. "font-variant-caps".
[[nodiscard]] std::string_view font_variant_property(FontVariantGroup group) noexcept;

} // namespace tessera::ui::style
