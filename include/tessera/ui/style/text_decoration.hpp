// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

// Value grammar of `text-decoration-*` and `vertical-align` (docs/design/text-decoration.md). One
// definition shared by the registry validator, `@supports`, the shorthand expansion and
// `text_style_from`, so a value the stylesheet accepts is a value the paint pass can draw.
namespace tessera::ui::style {

// Bits of the computed `text-decoration-line` set. `none` is the empty set.
inline constexpr std::uint8_t kDecorationUnderline = 1;
inline constexpr std::uint8_t kDecorationOverline = 2;
inline constexpr std::uint8_t kDecorationLineThrough = 4;

// `none | [underline || overline || line-through]`. `blink`, `spelling-error` and `grammar-error`
// are not drawable here, a repeated keyword is invalid CSS, and `none` cannot be combined, so each
// returns nullopt (the declaration is dropped with a diagnostic).
[[nodiscard]] std::optional<std::uint8_t> parse_text_decoration_line(std::string_view value);

// V1 draws `solid` only; `double`, `dotted`, `dashed` and `wavy` are rejected, not approximated.
[[nodiscard]] bool is_supported_text_decoration_style(std::string_view value);

// A non-negative `<length>` in px, em or rem (a bare `0` too). `font_size` is the base of em/rem.
// Percentages and every other unit return nullopt.
[[nodiscard]] std::optional<float> parse_decoration_length(std::string_view value, float font_size);

// `auto | from-font | <length>`. `auto` and `from-font` both mean "derive from the font size".
[[nodiscard]] bool is_supported_text_decoration_thickness(std::string_view value);

// The thickness in px, or 0 when it is `auto`/`from-font`/invalid (the painter derives it).
[[nodiscard]] float text_decoration_thickness(std::string_view value, float font_size);

// `baseline | sub | super | <length>`. `top`, `middle`, `bottom`, `text-top`, `text-bottom` and
// percentages depend on line-box geometry the engine does not model, so they are rejected.
[[nodiscard]] bool is_supported_vertical_align(std::string_view value);

// Baseline shift in px, positive raising the run. `sub` lowers by kSubShiftEm and `super` raises by
// kSuperShiftEm of `font_size` (UA-defined in CSS; the smaller size sub/sup text usually wants is
// the stylesheet's `font-size`, never guessed here). A length shifts by that amount. An invalid
// value is 0 (baseline).
inline constexpr float kSubShiftEm = 0.2f;
inline constexpr float kSuperShiftEm = 0.33f;
[[nodiscard]] float vertical_align_shift(std::string_view value, float font_size);

} // namespace tessera::ui::style
