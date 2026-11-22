// SPDX-License-Identifier: MIT

// `light-dark()` and `color-mix()` (CSS Color 5 sections 3 and 4), issues #620 and #621.
//
// Both are colour *functions* rather than colour syntaxes: they take other colours as arguments, so
// they evaluate by recursing into `parse_color`. That recursion is why they live in their own
// translation unit — `color.cpp` owns the fixed-channel grammars and must not grow a second
// responsibility past the file-size limit.

#include "color_functions.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/color_oklch.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style::detail {
namespace {

// The shared helpers (issue #733 item 1). This file kept its own `ascii_lower`, `is_space`, `trim`
// and `lower` after the migration, and its `lower` went through the locale table via `std::tolower`
// -- the exact defect the shared ASCII-only form exists to remove.
using tessera::detail::ascii_lower;
using tessera::detail::is_css_whitespace;
using tessera::detail::lower;
using tessera::detail::trim;

constexpr double kPi = 3.14159265358979323846;

// The function name and its argument text when `text` is exactly one call of `name`, else nullopt.
// `name` is compared ASCII-case-insensitively, as CSS function names are.
std::optional<std::string_view> call_arguments(std::string_view text, std::string_view name) {
    const std::string_view value = trim(text);
    if (value.size() <= name.size() + 1 || value.back() != ')') {
        return std::nullopt;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (ascii_lower(value[i]) != name[i]) {
            return std::nullopt;
        }
    }
    if (value[name.size()] != '(') {
        return std::nullopt;
    }
    // The closing parenthesis must be the last character: `light-dark(a, b) 4px` is not a colour.
    int depth = 0;
    for (std::size_t i = name.size(); i < value.size(); ++i) {
        if (value[i] == '(') {
            ++depth;
        } else if (value[i] == ')') {
            if (--depth == 0) {
                if (i + 1 != value.size()) {
                    return std::nullopt;
                }
                return value.substr(name.size() + 1, i - name.size() - 1);
            }
        }
    }
    return std::nullopt;
}

// Splits at `separator` outside parentheses, so a nested function stays one part.
std::vector<std::string_view> split_top_level(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size()) {
            if (text[i] == '(') {
                ++depth;
            } else if (text[i] == ')') {
                --depth;
            }
        }
        const bool at_separator = i < text.size() && text[i] == separator && depth == 0;
        if (i == text.size() || at_separator) {
            parts.push_back(trim(text.substr(start, i - start)));
            start = i + 1;
        }
    }
    return parts;
}

// A `<percentage>` or a bare number read as a percentage (CSS allows `0.5` for `50%` in a mix).
// Returns the percentage in 0..100 terms, or nullopt when the token is not a number.
std::optional<double> parse_percentage(std::string_view text) {
    std::string_view token = trim(text);
    bool percent = false;
    if (!token.empty() && token.back() == '%') {
        percent = true;
        token = trim(token.substr(0, token.size() - 1));
    }
    if (token.empty()) {
        return std::nullopt;
    }
    const std::string owned(token);
    char* end = nullptr;
    const double value = std::strtod(owned.c_str(), &end);
    if (end == nullptr || *end != '\0' || !std::isfinite(value)) {
        return std::nullopt;
    }
    return percent ? value : value * 100.0;
}

// One `color-mix()` operand: a colour plus its optional percentage.
struct MixOperand {
    ColorRgba color;
    std::optional<double> percentage;
};

// Splits `<color> <pct>?` where the colour may itself be a function with spaces inside it. The
// percentage, when present, is the last top-level whitespace-separated token and parses as a
// number.
std::optional<MixOperand> parse_operand(std::string_view text, const ColorContext& context,
                                        int depth) {
    const std::string_view value = trim(text);
    if (value.empty()) {
        return std::nullopt;
    }
    // Try the whole token first: `red`, `#fff`, `rgb(1 2 3)` and `light-dark(a, b)` all parse as a
    // colour with no percentage, and that is the common case.
    if (const ColorParseResult whole = parse_color(value, context, depth); whole.ok) {
        return MixOperand{whole.color, std::nullopt};
    }
    // Otherwise the last top-level space separates the colour from its percentage. Scanning forward
    // and remembering the last depth-zero space is what makes `rgb(1 2 3) 40%` split after the
    // function rather than inside it.
    int paren_depth = 0;
    std::size_t split = std::string_view::npos;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '(') {
            ++paren_depth;
        } else if (value[i] == ')') {
            --paren_depth;
        } else if (paren_depth == 0 && is_css_whitespace(value[i])) {
            split = i;
        }
    }
    if (split == std::string_view::npos) {
        return std::nullopt;
    }
    const std::optional<double> percentage = parse_percentage(value.substr(split + 1));
    if (!percentage) {
        return std::nullopt;
    }
    const ColorParseResult color = parse_color(trim(value.substr(0, split)), context, depth);
    if (!color.ok) {
        return std::nullopt;
    }
    return MixOperand{color.color, percentage};
}

// The interpolation spaces V1 accepts. `srgb` and `srgb-linear` interpolate the encoded and the
// linear-light channels; `oklab` and `oklch` go through the OKLab transform of `color_oklch.hpp`.
// Anything else (`lab`, `lch`, `hsl`, `hwb`, `xyz`) is rejected rather than silently treated as
// sRGB, so a stylesheet that asks for a space the engine cannot honour gets a diagnostic.
enum class MixSpace : std::uint8_t {
    Srgb,
    SrgbLinear,
    Oklab,
    Oklch,
};

std::optional<MixSpace> parse_space(std::string_view text) {
    const std::string name = lower(trim(text));
    if (name == "srgb") {
        return MixSpace::Srgb;
    }
    if (name == "srgb-linear") {
        return MixSpace::SrgbLinear;
    }
    if (name == "oklab") {
        return MixSpace::Oklab;
    }
    if (name == "oklch") {
        return MixSpace::Oklch;
    }
    return std::nullopt;
}

// Premultiplied interpolation of two colours in one space, then back to sRGB. `t` is the weight of
// `second` in 0..1. Premultiplying is what makes a mix with `transparent` keep the visible colour
// instead of darkening toward black, the same rule `interpolate_color` follows.
ColorRgba mix_in_space(const ColorRgba& first, const ColorRgba& second, double t, MixSpace space) {
    const double alpha = first.a + (second.a - first.a) * t;
    if (alpha <= 0.0) {
        return ColorRgba{second.r, second.g, second.b, 0.0f};
    }
    const auto blend = [&](double a, double b) {
        return (a * first.a + (b * second.a - a * first.a) * t) / alpha;
    };

    if (space == MixSpace::Srgb) {
        return ColorRgba{static_cast<float>(blend(first.r, second.r)),
                         static_cast<float>(blend(first.g, second.g)),
                         static_cast<float>(blend(first.b, second.b)), static_cast<float>(alpha)};
    }
    if (space == MixSpace::SrgbLinear) {
        const auto to_linear = [](float channel) { return srgb_to_linear(channel); };
        const auto to_encoded = [](double channel) { return linear_to_srgb(channel); };
        return ColorRgba{
            static_cast<float>(to_encoded(blend(to_linear(first.r), to_linear(second.r)))),
            static_cast<float>(to_encoded(blend(to_linear(first.g), to_linear(second.g)))),
            static_cast<float>(to_encoded(blend(to_linear(first.b), to_linear(second.b)))),
            static_cast<float>(alpha)};
    }

    // OKLab / OKLCH: convert both, interpolate the rectangular (L, a, b) form, convert back. OKLCH
    // interpolates the same rectangular pair rather than hue angles, which is what CSS Color 5
    // specifies for `oklch` (the hue is carried as a and b, so a mix never takes the long way round
    // the wheel by accident).
    const OklchColor first_ok = to_oklch(first);
    const OklchColor second_ok = to_oklch(second);
    const auto to_ab = [](const OklchColor& color, double& a, double& b) {
        const double radians = color.hue * kPi / 180.0;
        a = color.chroma * std::cos(radians);
        b = color.chroma * std::sin(radians);
    };
    double first_a = 0.0;
    double first_b = 0.0;
    double second_a = 0.0;
    double second_b = 0.0;
    to_ab(first_ok, first_a, first_b);
    to_ab(second_ok, second_a, second_b);

    OklchColor mixed;
    mixed.lightness = blend(first_ok.lightness, second_ok.lightness);
    const double a = blend(first_a, second_a);
    const double b = blend(first_b, second_b);
    mixed.chroma = std::sqrt(a * a + b * b);
    mixed.hue = std::atan2(b, a) * 180.0 / kPi;
    if (mixed.hue < 0.0) {
        mixed.hue += 360.0;
    }
    const ColorRgba rgb = to_srgb(mixed);
    return ColorRgba{rgb.r, rgb.g, rgb.b, static_cast<float>(alpha)};
}

std::optional<ColorRgba> evaluate_light_dark(std::string_view arguments,
                                             const ColorContext& context, int depth) {
    const std::vector<std::string_view> parts = split_top_level(arguments, ',');
    if (parts.size() != 2) {
        return std::nullopt;
    }
    // Both branches must be valid colours even though only one is used: CSS rejects the whole
    // function when either is invalid, so `light-dark(red, notacolor)` is not a colour in the light
    // scheme either. Validating both also keeps the result independent of the scheme, which is what
    // makes a theme's light and dark token sets fail together instead of only in dark mode.
    const ColorParseResult light = parse_color(parts[0], context, depth + 1);
    const ColorParseResult dark = parse_color(parts[1], context, depth + 1);
    if (!light.ok || !dark.ok) {
        return std::nullopt;
    }
    return context.dark ? dark.color : light.color;
}

std::optional<ColorRgba> evaluate_color_mix(std::string_view arguments, const ColorContext& context,
                                            int depth) {
    const std::vector<std::string_view> parts = split_top_level(arguments, ',');
    if (parts.size() != 3) {
        return std::nullopt;
    }
    // The first part is `in <space>`; CSS also allows `in <space> <hue-method>`, which V1 rejects
    // because none of the supported spaces is polar in the way a hue method changes.
    const std::vector<std::string_view> head = split_top_level(parts[0], ' ');
    if (head.size() != 2 || lower(head[0]) != "in") {
        return std::nullopt;
    }
    const std::optional<MixSpace> space = parse_space(head[1]);
    if (!space) {
        return std::nullopt;
    }
    const std::optional<MixOperand> first = parse_operand(parts[1], context, depth + 1);
    const std::optional<MixOperand> second = parse_operand(parts[2], context, depth + 1);
    if (!first || !second) {
        return std::nullopt;
    }

    // Percentage resolution (CSS Color 5 section 4.2): a missing percentage takes what is left of
    // 100%; two given percentages are normalized to sum to 100%; two missing ones are 50/50.
    double first_weight = 0.0;
    double second_weight = 0.0;
    if (first->percentage && second->percentage) {
        first_weight = *first->percentage;
        second_weight = *second->percentage;
        const double total = first_weight + second_weight;
        if (total <= 0.0) {
            return std::nullopt;
        }
        first_weight /= total;
        second_weight /= total;
    } else if (first->percentage) {
        first_weight = std::clamp(*first->percentage / 100.0, 0.0, 1.0);
        second_weight = 1.0 - first_weight;
    } else if (second->percentage) {
        second_weight = std::clamp(*second->percentage / 100.0, 0.0, 1.0);
        first_weight = 1.0 - second_weight;
    } else {
        first_weight = 0.5;
        second_weight = 0.5;
    }
    // `t` is the weight of the second colour; a zero weight of the first is the second colour.
    const double t = second_weight / (first_weight + second_weight);
    return mix_in_space(first->color, second->color, t, *space);
}

} // namespace

bool is_color_function(std::string_view text) {
    return call_arguments(text, "light-dark").has_value() ||
           call_arguments(text, "color-mix").has_value();
}

std::optional<ColorRgba> evaluate_color_function(std::string_view text, const ColorContext& context,
                                                 int depth) {
    if (const auto arguments = call_arguments(text, "light-dark")) {
        return evaluate_light_dark(*arguments, context, depth);
    }
    if (const auto arguments = call_arguments(text, "color-mix")) {
        return evaluate_color_mix(*arguments, context, depth);
    }
    return std::nullopt;
}

bool substitute_color_functions(std::string& value, const ColorContext& context) {
    // A value with no `(` cannot hold either function, and almost no declaration has one: scan
    // first so the common case allocates nothing.
    if (value.find('(') == std::string::npos) {
        return true;
    }
    // Bounded like `var()` substitution: a pathological value cannot make this loop forever.
    constexpr int kMaxPasses = 32;
    for (int pass = 0; pass < kMaxPasses; ++pass) {
        // Find the first call of either function at any depth. The outermost call is the one whose
        // opening parenthesis comes first, and evaluating it consumes everything nested inside.
        std::size_t start = std::string::npos;
        std::string_view name;
        for (std::size_t i = 0; i + 1 < value.size(); ++i) {
            if (value[i] != '(') {
                continue;
            }
            // Walk back over the identifier that precedes the parenthesis.
            std::size_t begin = i;
            while (begin > 0 && (std::isalnum(static_cast<unsigned char>(value[begin - 1])) != 0 ||
                                 value[begin - 1] == '-')) {
                --begin;
            }
            const std::string_view candidate(value.data() + begin, i - begin);
            if (candidate.size() == 10 && lower(candidate) == "light-dark") {
                start = begin;
                name = "light-dark";
                break;
            }
            if (candidate.size() == 9 && lower(candidate) == "color-mix") {
                start = begin;
                name = "color-mix";
                break;
            }
        }
        if (start == std::string::npos) {
            return true;
        }
        // Match the closing parenthesis of this call.
        int depth = 0;
        std::size_t close = std::string::npos;
        for (std::size_t i = start; i < value.size(); ++i) {
            if (value[i] == '(') {
                ++depth;
            } else if (value[i] == ')') {
                if (--depth == 0) {
                    close = i;
                    break;
                }
            }
        }
        if (close == std::string::npos) {
            return false;
        }
        const std::string_view call(value.data() + start, close - start + 1);
        const std::optional<ColorRgba> color = evaluate_color_function(call, context);
        if (!color) {
            return false;
        }
        value.replace(start, close - start + 1, format_color(*color));
    }
    return false;
}

} // namespace tessera::ui::style::detail
