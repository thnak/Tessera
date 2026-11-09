// SPDX-License-Identifier: MIT

#include "form_rules.hpp"

#include <algorithm>
#include <string>

namespace tessera::ui::forms::detail {
namespace {

std::size_t code_point_size(unsigned char lead) {
    if ((lead & 0x80U) == 0) {
        return 1;
    }
    if ((lead & 0xE0U) == 0xC0U) {
        return 2;
    }
    if ((lead & 0xF0U) == 0xE0U) {
        return 3;
    }
    if ((lead & 0xF8U) == 0xF0U) {
        return 4;
    }
    return 1;
}

RuleIssueTemplate issue(std::string_view code, std::string_view message) {
    return RuleIssueTemplate{std::string(code), std::string(message)};
}

} // namespace

std::size_t utf8_code_points(std::string_view text) {
    std::size_t count = 0;
    std::size_t index = 0;
    while (index < text.size()) {
        const std::size_t size = code_point_size(static_cast<unsigned char>(text[index]));
        index += std::min(size, text.size() - index);
        ++count;
    }
    return count;
}

bool glob_match(std::string_view pattern, std::string_view text) {
    std::size_t pattern_index = 0;
    std::size_t text_index = 0;
    std::size_t star_pattern = std::string_view::npos;
    std::size_t star_text = 0;
    while (text_index < text.size()) {
        const bool literal = pattern_index < pattern.size() && pattern[pattern_index] != '*' &&
                             pattern[pattern_index] != '?' &&
                             static_cast<unsigned char>(pattern[pattern_index]) ==
                                 static_cast<unsigned char>(text[text_index]);
        if (pattern_index < pattern.size() && pattern[pattern_index] == '?') {
            const std::size_t size = code_point_size(static_cast<unsigned char>(text[text_index]));
            const std::size_t advance = std::min(size, text.size() - text_index);
            pattern_index += 1;
            text_index += advance;
            continue;
        }
        if (literal) {
            ++pattern_index;
            ++text_index;
            continue;
        }
        if (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
            star_pattern = pattern_index;
            star_text = text_index;
            ++pattern_index;
            continue;
        }
        if (star_pattern != std::string_view::npos) {
            // Backtrack to the last '*': it swallows one more code point.
            pattern_index = star_pattern + 1;
            const std::size_t size = code_point_size(static_cast<unsigned char>(text[star_text]));
            star_text += std::min(size, text.size() - star_text);
            text_index = star_text;
            continue;
        }
        return false;
    }
    while (pattern_index < pattern.size() && pattern[pattern_index] == '*') {
        ++pattern_index;
    }
    return pattern_index == pattern.size();
}

std::optional<std::int64_t> parse_integer(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        index = 1;
    } else if (text[0] == '+') {
        index = 1;
    }
    if (index == text.size()) {
        return std::nullopt;
    }
    std::int64_t value = 0;
    const std::int64_t limit = negative ? 9223372036854775807LL : 9223372036854775807LL;
    for (; index < text.size(); ++index) {
        const char character = text[index];
        if (character < '0' || character > '9') {
            return std::nullopt;
        }
        const int digit = character - '0';
        if (value > (limit - digit) / 10) {
            return std::nullopt;
        }
        value = value * 10 + digit;
    }
    return negative ? -value : value;
}

std::optional<RuleIssueTemplate> evaluate_sync_rule(const SyncRule& rule, const FormValue& value,
                                                    const FormValue* equal_to_value) {
    switch (rule.kind) {
    case RuleKind::Required:
        if (value.kind == FormValueKind::String) {
            if (value.text.empty()) {
                return issue("required", "Required");
            }
            return std::nullopt;
        }
        if (value.kind == FormValueKind::Bool) {
            if (!value.bool_value) {
                return issue("required", "Required");
            }
            return std::nullopt;
        }
        if (value.enum_value < 0) {
            return issue("required", "Required");
        }
        return std::nullopt;
    case RuleKind::MinLength:
    case RuleKind::MaxLength: {
        if (value.kind != FormValueKind::String) {
            return issue("length_expects_string", "Length rules apply to text fields");
        }
        const std::size_t length = utf8_code_points(value.text);
        if (rule.kind == RuleKind::MinLength && length < static_cast<std::size_t>(rule.minimum)) {
            return issue("min_length", "Too short");
        }
        if (rule.kind == RuleKind::MaxLength && length > static_cast<std::size_t>(rule.maximum)) {
            return issue("max_length", "Too long");
        }
        return std::nullopt;
    }
    case RuleKind::Pattern:
        if (value.kind != FormValueKind::String) {
            return issue("pattern_expects_string", "Pattern rules apply to text fields");
        }
        if (glob_match(rule.pattern, value.text)) {
            return std::nullopt;
        }
        return issue("pattern", "Does not match the pattern");
    case RuleKind::NumericRange: {
        std::optional<std::int64_t> number;
        if (value.kind == FormValueKind::Enum) {
            number = value.enum_value;
        } else if (value.kind == FormValueKind::String) {
            number = parse_integer(value.text);
            if (!number.has_value()) {
                return issue("not_numeric", "Not a number");
            }
        } else {
            return issue("numeric_expects_number", "Range rules apply to numeric fields");
        }
        if (*number < rule.minimum || *number > rule.maximum) {
            return issue("numeric_range", "Out of range");
        }
        return std::nullopt;
    }
    case RuleKind::EqualTo:
        if (equal_to_value == nullptr) {
            return issue("equal_to_missing", "Comparison field does not exist");
        }
        if (value == *equal_to_value) {
            return std::nullopt;
        }
        return issue("equal_to", "Values do not match");
    case RuleKind::Custom:
        if (!rule.custom) {
            return issue("custom_missing", "Custom rule is not configured");
        }
        try {
            return rule.custom(value);
        } catch (...) {
            // Rules are values: a throwing custom callback becomes an issue, never an escape.
            return issue("custom_exception", "Validation failed");
        }
    }
    return issue("unknown_rule", "Unknown validation rule");
}

} // namespace tessera::ui::forms::detail
