// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/style/color.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace tessera::ui::style {

// One function of a parsed `filter` value (issue #617).
struct FilterFunction {
    enum class Kind : std::uint8_t { Blur, DropShadow };
    Kind kind = Kind::Blur;
    float offset_x = 0.0f; // CSS px, DropShadow only
    float offset_y = 0.0f;
    float sigma = 0.0f; // CSS px; the third `drop-shadow()` length is a standard deviation
    ColorRgba color{0.0f, 0.0f, 0.0f, 1.0f};
    bool current_color = false; // DropShadow without a colour (or `currentcolor`): the text colour

    [[nodiscard]] bool operator==(const FilterFunction&) const = default;
};

// V1 bound on the folded list: adjacent `blur()`s fold exactly (Gaussians convolve, variances
// add), so this is the number of alternations a declaration can carry. The layer chain the
// renderer runs has the same size.
inline constexpr std::size_t kMaxFilterFunctions = 3;

struct Filter {
    std::array<FilterFunction, kMaxFilterFunctions> functions{};
    std::size_t count = 0;

    [[nodiscard]] bool none() const {
        return count == 0;
    }
    [[nodiscard]] bool operator==(const Filter&) const = default;
};

struct FilterParseResult {
    Filter filter;
    // Empty on success; otherwise a static reason, and `token` names the offending text.
    std::string_view error;
    std::string_view token;

    [[nodiscard]] bool ok() const {
        return error.empty();
    }
};

// Parses the V1 `filter` grammar:
//
//     none | <filter-function>+
//     <filter-function> = blur(<length>) | drop-shadow(<color>? && <length>{2,3})
//
// `<length>` is a bare number or `px`; a blur length may not be negative, offsets may. A blur of 0
// and a fully transparent drop-shadow with no blur are dropped (they change nothing). Any other
// function (brightness, hue-rotate, url(), ...) makes the whole declaration invalid, like
// `backdrop-filter`.
[[nodiscard]] FilterParseResult parse_filter(std::string_view value);

} // namespace tessera::ui::style
