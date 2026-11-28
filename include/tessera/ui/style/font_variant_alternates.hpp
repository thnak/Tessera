// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

// The `font-variant-alternates` grammar (CSS Fonts 4 section 6.6, issue #1011; docs/design/text.md
// section 3.1): `normal | [ stylistic(n) || historical-forms || styleset(n#) ||
// character-variant(n#)
// || swash(n) || ornaments(n) || annotation(n) ]`. Each function appears at most once. The style
// layer judges the grammar and maps it to OpenType settings; the text layer applies them.
namespace tessera::ui::style {

inline constexpr std::string_view kFontVariantAlternatesProperty = "font-variant-alternates";

// One OpenType feature an alternates function selects (the FontVariantSetting shape, wider value).
struct FontVariantAlternateSetting {
    char tag[5];
    std::uint32_t value;
};

// Parses a whole value (case-insensitive). `normal` is valid and yields no settings. A function
// argument is a non-negative integer feature index (`swash(2)`, `styleset(1, 3)`) or an identifier
// naming a feature-value alias; an alias has no @font-feature-values table to resolve against in
// this engine (a documented follow-up), so it is valid and selects nothing, as CSS does for an
// unresolved name. Empty (nullopt) on an unknown word, a repeated function, an empty or malformed
// argument list, or a stray token.
[[nodiscard]] std::optional<std::vector<FontVariantAlternateSetting>>
parse_font_variant_alternates(std::string_view value);

// True for the words that start an alternates component in a `font-variant` shorthand value: the
// bare `historical-forms` and the six function names (matched on the name before `(`).
[[nodiscard]] bool is_font_variant_alternates_word(std::string_view lowered_word);

} // namespace tessera::ui::style
