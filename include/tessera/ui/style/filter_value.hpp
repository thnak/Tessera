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
    enum class Kind : std::uint8_t {
        Blur,
        DropShadow,
        // The colour functions (Filter Effects Module Level 1): `amount` is the computed value.
        Brightness, // factor >= 0
        Contrast,   // factor >= 0
        Grayscale,  // 0..1
        Sepia,      // 0..1
        Saturate,   // factor >= 0
        HueRotate,  // degrees, the declared angle converted (not wrapped)
        Invert,     // 0..1
        Opacity,    // 0..1
    };
    Kind kind = Kind::Blur;
    float amount = 0.0f;   // colour functions only
    float offset_x = 0.0f; // CSS px, DropShadow only
    float offset_y = 0.0f;
    float sigma = 0.0f; // CSS px; the third `drop-shadow()` length is a standard deviation
    ColorRgba color{0.0f, 0.0f, 0.0f, 1.0f};
    bool current_color = false; // DropShadow without a colour (or `currentcolor`): the text colour

    // Written out because ColorRgba has no operator==, so a defaulted one is deleted (clang rejects
    // that under -Werror=defaulted-function-deleted).
    [[nodiscard]] bool is_color_function() const {
        return kind != Kind::Blur && kind != Kind::DropShadow;
    }

    [[nodiscard]] bool operator==(const FilterFunction& other) const {
        return kind == other.kind && amount == other.amount && offset_x == other.offset_x &&
               offset_y == other.offset_y && sigma == other.sigma && color.r == other.color.r &&
               color.g == other.color.g && color.b == other.color.b && color.a == other.color.a &&
               current_color == other.current_color;
    }
};

// V1 bounds. A declaration folds adjacent `blur()`s exactly (Gaussians convolve, variances add) and
// the renderer runs up to three colour functions in one pass, so a declaration may carry
// `kMaxFilterOps` blur, drop-shadow or colour-function runs: the layer chain the renderer runs has
// the same size (paint_filter.cpp asserts it). `kMaxFilterFunctions` bounds the written list.
inline constexpr std::size_t kMaxFilterOps = 3;
inline constexpr std::size_t kColorFunctionsPerOp = 3;
inline constexpr std::size_t kMaxFilterFunctions = kMaxFilterOps * kColorFunctionsPerOp;

struct Filter {
    std::array<FilterFunction, kMaxFilterFunctions> functions{};
    std::size_t count = 0;

    [[nodiscard]] bool none() const {
        return count == 0;
    }
    // How many renderer ops the list needs: a run of up to kColorFunctionsPerOp adjacent colour
    // functions shares one, every other function is its own.
    [[nodiscard]] std::size_t op_count() const;
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
//                       | brightness(<amount>?) | contrast(<amount>?) | saturate(<amount>?)
//                       | grayscale(<amount>?) | sepia(<amount>?) | invert(<amount>?)
//                       | opacity(<amount>?) | hue-rotate(<angle>?)
//
// `<length>` is a bare number or `px`; a blur length may not be negative, offsets may. `<amount>`
// is a number or a percentage and may not be negative (the argument may be omitted: it is 1, so
// `grayscale()` is the full effect and only `brightness()`, `contrast()`, `saturate()` and
// `opacity()` are identities);
// grayscale, sepia, invert and opacity clamp above 1 (100%). `<angle>` is deg, grad, rad or turn,
// or a bare 0. These rules are measured in Chromium (tests/unit/filter_color_tests.cpp). A function
// that changes nothing (blur(0), brightness(1), grayscale(0), hue-rotate(360deg), a transparent
// unblurred shadow) is dropped. A list needing more than kMaxFilterOps renderer ops, `calc()` and
// any other function (url(), ...) make the whole declaration invalid, like `backdrop-filter`.
[[nodiscard]] FilterParseResult parse_filter(std::string_view value);

} // namespace tessera::ui::style
