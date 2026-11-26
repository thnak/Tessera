// SPDX-License-Identifier: MIT

#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/style_environment.hpp"
#include "var_scan.hpp"

#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::style::detail {

char ascii_lower(char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
}

bool is_custom_property_name(std::string_view name) {
    if (name.size() < 3 || name[0] != '-' || name[1] != '-') {
        return false;
    }
    for (std::size_t i = 2; i < name.size(); ++i) {
        const unsigned char c = static_cast<unsigned char>(name[i]);
        if (c >= 0x80) {
            continue; // non-ASCII identifiers are legal in CSS
        }
        if (std::isalnum(c) == 0 && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

// `var(` matches ASCII-case-insensitively (CSS function names are case-insensitive).
bool is_var_function(std::string_view value, std::size_t at) {
    return at + 4 <= value.size() && ascii_lower(value[at]) == 'v' &&
           ascii_lower(value[at + 1]) == 'a' && ascii_lower(value[at + 2]) == 'r' &&
           value[at + 3] == '(';
}

bool is_env_function(std::string_view value, std::size_t at) {
    return at + 4 <= value.size() && ascii_lower(value[at]) == 'e' &&
           ascii_lower(value[at + 1]) == 'n' && ascii_lower(value[at + 2]) == 'v' &&
           value[at + 3] == '(';
}

// A `var()` or `env()` call, outside strings: either one is substituted at computed-value time.
bool contains_var_function(std::string_view value) {
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] == '"' || value[i] == '\'') {
            const char quote = value[i];
            for (++i; i < value.size(); ++i) {
                if (value[i] == '\\' && i + 1 < value.size()) {
                    ++i;
                } else if (value[i] == quote) {
                    break;
                }
            }
            continue;
        }
        if (is_var_function(value, i) || is_env_function(value, i)) {
            return true;
        }
    }
    return false;
}

std::size_t skip_string(std::string_view value, std::size_t at) {
    const char quote = value[at];
    for (std::size_t i = at + 1; i < value.size(); ++i) {
        if (value[i] == '\\' && i + 1 < value.size()) {
            ++i;
        } else if (value[i] == quote) {
            return i + 1;
        }
    }
    return value.size();
}

std::size_t match_paren(std::string_view value, std::size_t open) {
    int depth = 0;
    for (std::size_t i = open; i < value.size(); ++i) {
        if (value[i] == '"' || value[i] == '\'') {
            i = skip_string(value, i) - 1;
        } else if (value[i] == '(') {
            ++depth;
        } else if (value[i] == ')') {
            if (--depth == 0) {
                return i;
            }
        }
    }
    return std::string_view::npos;
}

// First comma at nesting depth zero: it separates the custom property name from the fallback.
std::size_t find_comma(std::string_view text) {
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '"' || c == '\'') {
            i = skip_string(text, i) - 1;
        } else if (c == '(' || c == '[') {
            ++depth;
        } else if (c == ')' || c == ']') {
            --depth;
        } else if (c == ',' && depth == 0) {
            return i;
        }
    }
    return std::string_view::npos;
}

std::string trim(std::string_view text) {
    return std::string(tessera::detail::trim(text));
}

// The name of an `env()` variable with its arguments: tokens separated by single spaces, so
// `viewport-segment-width   0  1` and `viewport-segment-width 0 1` are the same variable.
std::string normalize_env_name(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    bool pending_space = false;
    for (const char c : text) {
        if (std::isspace(static_cast<unsigned char>(c)) != 0) {
            pending_space = !out.empty();
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(c);
    }
    return out;
}

} // namespace tessera::ui::style::detail

namespace tessera::ui::style {

void collect_env_references(std::string_view value, std::vector<std::string>& names) {
    using namespace detail;
    for (std::size_t i = 0; i < value.size();) {
        if (value[i] == '"' || value[i] == '\'') {
            i = skip_string(value, i);
            continue;
        }
        if (!is_env_function(value, i)) {
            ++i;
            continue;
        }
        const std::size_t close = match_paren(value, i + 3);
        if (close == std::string_view::npos) {
            return;
        }
        const std::string_view inner = value.substr(i + 4, close - i - 4);
        const std::size_t comma = find_comma(inner);
        names.push_back(normalize_env_name(inner.substr(0, comma)));
        if (comma != std::string_view::npos) {
            collect_env_references(inner.substr(comma + 1), names);
        }
        i = close + 1;
    }
}

} // namespace tessera::ui::style
