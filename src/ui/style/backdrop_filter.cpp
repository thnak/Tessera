// SPDX-License-Identifier: MIT

#include "tessera/ui/style/backdrop_filter.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cmath>
#include <optional>
#include <string_view>
#include <system_error>

namespace tessera::ui::style {
namespace {

constexpr std::string_view kExpectedFunction =
    "expected `none` or a backdrop filter function such as `blur(8px)`";
constexpr std::string_view kUnknownFunction = "not a supported backdrop-filter function";
constexpr std::string_view kBadLength =
    "expected a non-negative length in px, for example `blur(8px)`";
constexpr std::string_view kBadFactor = "expected a non-negative number or percentage";
constexpr std::string_view kTooMany = "too many backdrop-filter functions";

std::string_view trim_left(std::string_view text) {
    std::size_t first = 0;
    while (first < text.size() && std::isspace(static_cast<unsigned char>(text[first])) != 0) {
        ++first;
    }
    return text.substr(first);
}

std::string_view trim(std::string_view text) {
    text = trim_left(text);
    std::size_t last = text.size();
    while (last > 0 && std::isspace(static_cast<unsigned char>(text[last - 1])) != 0) {
        --last;
    }
    return text.substr(0, last);
}

bool equals_ignore_case(std::string_view lhs, std::string_view rhs) {
    if (lhs.size() != rhs.size()) {
        return false;
    }
    for (std::size_t index = 0; index < lhs.size(); ++index) {
        if (std::tolower(static_cast<unsigned char>(lhs[index])) !=
            std::tolower(static_cast<unsigned char>(rhs[index]))) {
            return false;
        }
    }
    return true;
}

// Parses one CSS number at the start of `text` and reports the unit suffix it stopped at. A leading
// `+` is accepted (CSS allows it; `std::from_chars` does not). NaN/inf and out-of-range literals
// are rejected: they are not CSS numbers.
bool parse_number_prefix(std::string_view text, double& value, std::string_view& suffix) {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.empty()) {
        return false;
    }
    const char* first = text.data();
    const char* last = text.data() + text.size();
    const auto result = std::from_chars(first, last, value);
    if (result.ec != std::errc{} || !std::isfinite(value)) {
        return false;
    }
    suffix = text.substr(static_cast<std::size_t>(result.ptr - first));
    return true;
}

bool parse_length(std::string_view text, double& value) {
    std::string_view suffix;
    if (!parse_number_prefix(text, value, suffix)) {
        return false;
    }
    // V1 length rule (the same one paint reads for other properties): a bare number or `px`.
    return suffix.empty() || equals_ignore_case(suffix, "px");
}

bool parse_factor(std::string_view text, double& value) {
    std::string_view suffix;
    if (!parse_number_prefix(text, value, suffix)) {
        return false;
    }
    if (suffix.empty()) {
        return true;
    }
    if (suffix != "%") {
        return false;
    }
    value *= 0.01;
    return true;
}

BackdropFilterParseResult failure(std::string_view error, std::string_view token = {}) {
    return BackdropFilterParseResult{BackdropFilter{}, error, token};
}

struct Function {
    std::string_view name;
    std::string_view argument;
};

// Reads `<ident>(<argument>)` from the front of `text`. The argument may not contain parentheses:
// the glass grammar has no nested function, so `calc()` and `var()` are invalid here (a `var()`
// reference is resolved before the computed value is parsed). On failure `text` is left untouched,
// so the caller can still name the token it rejected.
std::optional<Function> read_function(std::string_view& text) {
    std::string_view rest = text;
    std::size_t name_length = 0;
    while (name_length < rest.size()) {
        const char c = rest[name_length];
        if (std::isalpha(static_cast<unsigned char>(c)) == 0 && c != '-') {
            break;
        }
        ++name_length;
    }
    if (name_length == 0) {
        return std::nullopt;
    }
    Function function;
    function.name = rest.substr(0, name_length);
    rest.remove_prefix(name_length);
    rest = trim_left(rest);
    if (rest.empty() || rest.front() != '(') {
        return std::nullopt;
    }
    const std::size_t close = rest.find(')');
    if (close == std::string_view::npos) {
        return std::nullopt;
    }
    const std::string_view inner = rest.substr(1, close - 1);
    if (inner.find('(') != std::string_view::npos) {
        return std::nullopt;
    }
    function.argument = trim(inner);
    text = rest.substr(close + 1);
    return function;
}

} // namespace

BackdropFilterParseResult parse_backdrop_filter(std::string_view value) {
    std::string_view rest = trim(value);
    if (rest.empty()) {
        return failure(kExpectedFunction);
    }
    if (equals_ignore_case(rest, "none")) {
        return BackdropFilterParseResult{};
    }

    BackdropFilter filter;
    std::size_t count = 0;
    while (true) {
        rest = trim(rest);
        if (rest.empty()) {
            break;
        }
        if (++count > kMaxBackdropFilterFunctions) {
            return failure(kTooMany);
        }
        const std::optional<Function> function = read_function(rest);
        if (!function.has_value()) {
            // Name the first token so the diagnostic can say which keyword is not a function.
            std::size_t token_length = 0;
            while (token_length < rest.size() &&
                   std::isspace(static_cast<unsigned char>(rest[token_length])) == 0) {
                ++token_length;
            }
            return failure(kExpectedFunction, rest.substr(0, token_length));
        }

        const std::string_view name = function->name;
        if (equals_ignore_case(name, "blur")) {
            double length = 0.0;
            if (!parse_length(function->argument, length) || length < 0.0) {
                return failure(kBadLength, name);
            }
            // Convolving two Gaussians adds their variances, so repeated `blur()` folds exactly.
            const float radius = static_cast<float>(length);
            filter.blur_sigma = std::sqrt(filter.blur_sigma * filter.blur_sigma + radius * radius);
            continue;
        }

        float* target = nullptr;
        if (equals_ignore_case(name, "saturate")) {
            target = &filter.saturate;
        } else if (equals_ignore_case(name, "brightness")) {
            target = &filter.brightness;
        } else if (equals_ignore_case(name, "contrast")) {
            target = &filter.contrast;
        } else {
            return failure(kUnknownFunction, name);
        }
        double factor = 0.0;
        if (!parse_factor(function->argument, factor) || factor < 0.0) {
            return failure(kBadFactor, name);
        }
        *target *= static_cast<float>(factor);
    }

    if (count == 0) {
        return failure(kExpectedFunction);
    }
    return BackdropFilterParseResult{filter, {}, {}};
}

} // namespace tessera::ui::style
