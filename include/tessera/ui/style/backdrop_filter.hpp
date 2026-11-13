// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <string_view>

namespace tessera::ui::style {

// A parsed `backdrop-filter` value (glass-v1 section 2). The written list is folded here:
//
// - `blur(<length>)` composes two Gaussians, so repeated blurs fold as
//   sigma = sqrt(sigma_a^2 + sigma_b^2) - the exact result of convolving them;
// - `saturate`/`brightness`/`contrast` are multiplicative and commute, so folding in written order
//   and folding in any other order give the same matrix (as in CSS).
//
// The value is a pass-through when `none()` is true. The style layer never clamps `blur_sigma` to
// the runtime `glassMaxBlurRadius`; that clamp is applied where the limit is known (the paint pass,
// which carries the configured limit), so a stylesheet compiles once and behaves per config.
struct BackdropFilter {
    float blur_sigma = 0.0f;
    float saturate = 1.0f;
    float brightness = 1.0f;
    float contrast = 1.0f;

    [[nodiscard]] bool none() const {
        return blur_sigma <= 0.0f && saturate == 1.0f && brightness == 1.0f && contrast == 1.0f;
    }
    [[nodiscard]] bool operator==(const BackdropFilter&) const = default;
};

// Sanity bound on one declaration's function list (a stylesheet cannot make the paint pass fold an
// unbounded list). CSS itself has no small limit; eight is far beyond a real glass declaration.
inline constexpr std::size_t kMaxBackdropFilterFunctions = 8;

struct BackdropFilterParseResult {
    BackdropFilter filter;
    // Empty on success. Otherwise a static, human-readable reason; `token` names the offending
    // text (borrowed from the parsed value) when one exists.
    std::string_view error;
    std::string_view token;

    [[nodiscard]] bool ok() const {
        return error.empty();
    }
};

// Parses the `backdrop-filter` / `-webkit-backdrop-filter` grammar of glass-v1 section 2:
//
//     none | <filter-function>+
//     <filter-function> = blur(<length>) | saturate(<number>|<percent>)
//                       | brightness(<number>|<percent>) | contrast(<number>|<percent>)
//
// `<length>` accepts a bare number or a `px` suffix (the stylesheet engine's V1 length rule; `em`,
// `rem`, `vw` and percentages are invalid). A negative or non-finite blur length/number is invalid.
// Any other function (sepia, hue-rotate, drop-shadow, ...) is invalid: the caller reports
// `UnsupportedValue` and ignores the whole declaration (glass-v1 section 2), never half of it.
[[nodiscard]] BackdropFilterParseResult parse_backdrop_filter(std::string_view value);

} // namespace tessera::ui::style
