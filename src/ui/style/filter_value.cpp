// SPDX-License-Identifier: MIT

#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/filter_value.hpp"

#include <charconv>
#include <cmath>
#include <optional>
#include <string>
#include <system_error>

namespace tessera::ui::style {

using tessera::detail::ascii_lower;
using tessera::detail::is_css_whitespace;
using tessera::detail::lower;
using tessera::detail::split_top_level;
using tessera::detail::trim;
namespace {

constexpr std::string_view kExpected =
    "expected `none` or filter functions such as `blur(4px)` or `drop-shadow(2px 2px 4px black)`";
constexpr std::string_view kUnknown = "not a supported filter function (blur and drop-shadow only)";
constexpr std::string_view kBadBlur = "expected a non-negative length, for example `blur(4px)`";
constexpr std::string_view kBadShadow =
    "expected `drop-shadow(<color>? <x> <y> <blur>?)` with px lengths and a non-negative blur";
constexpr std::string_view kTooMany = "too many filter functions after folding adjacent blur()";

// A CSS number with an optional `px` suffix. A bare number is accepted (the engine's V1 length
// rule); NaN, inf and any other unit are not lengths.
bool parse_length(std::string_view text, float& value) {
    text = trim(text);
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    double parsed = 0.0;
    const char* const first = text.data();
    const auto result = std::from_chars(first, first + text.size(), parsed);
    if (text.empty() || result.ec != std::errc{} || !std::isfinite(parsed)) {
        return false;
    }
    const std::string_view suffix = text.substr(static_cast<std::size_t>(result.ptr - first));
    if (!suffix.empty() && lower(suffix) != "px") {
        return false;
    }
    value = static_cast<float>(parsed);
    return true;
}

bool is_length_token(std::string_view token) {
    if (token.empty()) {
        return false;
    }
    const char c = token.front();
    return (c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.';
}

struct Call {
    std::string_view name;
    std::string_view argument;
};

// Reads `<ident>(<argument>)` from the front of `text`; the argument may nest parentheses (a
// colour function). On failure `text` is untouched so the caller can name the token.
std::optional<Call> read_call(std::string_view& text) {
    std::size_t length = 0;
    while (length < text.size()) {
        const char c = ascii_lower(text[length]);
        if ((c < 'a' || c > 'z') && c != '-') {
            break;
        }
        ++length;
    }
    std::size_t open = length;
    while (open < text.size() && is_css_whitespace(text[open])) {
        ++open;
    }
    if (length == 0 || open >= text.size() || text[open] != '(') {
        return std::nullopt;
    }
    int depth = 0;
    for (std::size_t i = open; i < text.size(); ++i) {
        depth += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
        if (depth == 0) {
            Call call{text.substr(0, length), trim(text.substr(open + 1, i - open - 1))};
            text.remove_prefix(i + 1);
            return call;
        }
    }
    return std::nullopt;
}

bool parse_drop_shadow(std::string_view argument, FilterFunction& out) {
    out = FilterFunction{};
    out.kind = FilterFunction::Kind::DropShadow;
    out.current_color = true;
    float lengths[3] = {};
    int length_count = 0;
    bool have_color = false;
    for (const std::string_view token : split_top_level(argument, ' ')) {
        if (trim(token).empty()) {
            continue;
        }
        if (is_length_token(token)) {
            if (length_count == 3 || !parse_length(token, lengths[length_count])) {
                return false;
            }
            ++length_count;
        } else if (have_color) {
            return false;
        } else if (lower(trim(token)) == "currentcolor") {
            have_color = true;
        } else {
            const ColorParseResult parsed = parse_color(trim(token));
            if (!parsed.ok) {
                return false;
            }
            have_color = true;
            out.current_color = false;
            out.color = parsed.color;
        }
    }
    if (length_count < 2 || (length_count == 3 && lengths[2] < 0.0f)) {
        return false;
    }
    out.offset_x = lengths[0];
    out.offset_y = lengths[1];
    out.sigma = length_count == 3 ? lengths[2] : 0.0f;
    return true;
}

FilterParseResult failure(std::string_view error, std::string_view token = {}) {
    return FilterParseResult{Filter{}, error, token};
}

} // namespace

FilterParseResult parse_filter(std::string_view value) {
    std::string_view rest = trim(value);
    if (rest.empty()) {
        return failure(kExpected);
    }
    if (lower(rest) == "none") {
        return FilterParseResult{};
    }
    Filter filter;
    std::size_t written = 0;
    bool any = false;
    while (!(rest = trim(rest)).empty()) {
        const std::optional<Call> call = read_call(rest);
        if (!call.has_value()) {
            return failure(kExpected, rest.substr(0, rest.find_first_of(" \t\r\n\f")));
        }
        any = true;
        const std::string name = lower(call->name);
        FilterFunction function;
        if (name == "blur") {
            float length = 0.0f;
            if (!parse_length(call->argument, length) || length < 0.0f) {
                return failure(kBadBlur, call->name);
            }
            function.kind = FilterFunction::Kind::Blur;
            function.sigma = length;
            // Gaussians convolve, so adjacent blurs fold exactly: sigma^2 adds.
            if (written > 0 && filter.functions[written - 1].kind == FilterFunction::Kind::Blur) {
                FilterFunction& previous = filter.functions[written - 1];
                previous.sigma = std::sqrt(previous.sigma * previous.sigma + length * length);
                continue;
            }
        } else if (name == "drop-shadow") {
            if (!parse_drop_shadow(call->argument, function)) {
                return failure(kBadShadow, call->name);
            }
        } else {
            return failure(kUnknown, call->name);
        }
        if (written == kMaxFilterFunctions) {
            return failure(kTooMany, call->name);
        }
        filter.functions[written++] = function;
    }
    if (!any) {
        return failure(kExpected);
    }
    // A function that changes nothing is dropped so `blur(0)` and `none` paint identically (no
    // layer): a zero blur, and a shadow that is invisible and unblurred.
    std::size_t kept = 0;
    for (std::size_t index = 0; index < written; ++index) {
        const FilterFunction& function = filter.functions[index];
        const bool blur_noop =
            function.kind == FilterFunction::Kind::Blur && function.sigma <= 0.0f;
        const bool shadow_noop = function.kind == FilterFunction::Kind::DropShadow &&
                                 !function.current_color && function.color.a <= 0.0f;
        if (!blur_noop && !shadow_noop) {
            filter.functions[kept++] = function;
        }
    }
    filter.count = kept;
    return FilterParseResult{filter, {}, {}};
}

} // namespace tessera::ui::style
