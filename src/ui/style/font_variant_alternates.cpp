// SPDX-License-Identifier: MIT

#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/font_variant_alternates.hpp"

#include <array>
#include <cstddef>
#include <string>

namespace tessera::ui::style {
namespace {

enum class Function : std::uint8_t {
    Stylistic,
    Swash,
    Ornaments,
    Annotation,
    Styleset,
    CharacterVariant,
    Historical,
    kCount,
};

constexpr std::uint32_t kMaxIndex = 0xFFFFU;

bool is_blank(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

std::optional<Function> function_named(std::string_view name) {
    if (name == "stylistic") {
        return Function::Stylistic;
    }
    if (name == "swash") {
        return Function::Swash;
    }
    if (name == "ornaments") {
        return Function::Ornaments;
    }
    if (name == "annotation") {
        return Function::Annotation;
    }
    if (name == "styleset") {
        return Function::Styleset;
    }
    if (name == "character-variant") {
        return Function::CharacterVariant;
    }
    return std::nullopt;
}

bool is_ident(std::string_view text) {
    if (text.empty() || !((text[0] >= 'a' && text[0] <= 'z') || text[0] == '_')) {
        return false;
    }
    for (const char c : text) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// One argument: an index (value set) or an alias ident (no index, valid but unresolved).
struct Argument {
    bool has_index = false;
    std::uint32_t index = 0;
};

std::optional<Argument> parse_argument(std::string_view text) {
    Argument argument;
    if (text.empty()) {
        return std::nullopt;
    }
    if (text[0] >= '0' && text[0] <= '9') {
        for (const char c : text) {
            if (c < '0' || c > '9' || argument.index > kMaxIndex) {
                return std::nullopt;
            }
            argument.index = argument.index * 10U + static_cast<std::uint32_t>(c - '0');
        }
        if (argument.index > kMaxIndex) {
            return std::nullopt;
        }
        argument.has_index = true;
        return argument;
    }
    return is_ident(text) ? std::optional<Argument>(argument) : std::nullopt;
}

std::optional<std::vector<Argument>> parse_arguments(std::string_view list) {
    std::vector<Argument> out;
    std::size_t begin = 0;
    while (true) {
        const std::size_t comma = list.find(',', begin);
        const std::string_view piece = tessera::detail::trim(
            list.substr(begin, comma == std::string_view::npos ? comma : comma - begin));
        const auto argument = parse_argument(piece);
        if (!argument) {
            return std::nullopt;
        }
        out.push_back(*argument);
        if (comma == std::string_view::npos) {
            return out;
        }
        begin = comma + 1;
    }
}

void add(std::vector<FontVariantAlternateSetting>& out, const char (&tag)[5], std::uint32_t value) {
    FontVariantAlternateSetting setting{{tag[0], tag[1], tag[2], tag[3], '\0'}, value};
    out.push_back(setting);
}

void add_numbered(std::vector<FontVariantAlternateSetting>& out, char a, char b,
                  std::uint32_t number) {
    const char tag[5] = {a, b, static_cast<char>('0' + number / 10U),
                         static_cast<char>('0' + number % 10U), '\0'};
    out.push_back(FontVariantAlternateSetting{{tag[0], tag[1], tag[2], tag[3], '\0'}, 1});
}

// Splits at top-level blanks (parentheses keep their spaces: `styleset(1, 2)`).
std::optional<std::vector<std::string_view>> split_tokens(std::string_view text) {
    std::vector<std::string_view> tokens;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && is_blank(text[i])) {
            ++i;
        }
        const std::size_t begin = i;
        int depth = 0;
        while (i < text.size() && (depth > 0 || !is_blank(text[i]))) {
            depth += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
            if (depth < 0) {
                return std::nullopt;
            }
            ++i;
        }
        if (depth != 0) {
            return std::nullopt;
        }
        if (i > begin) {
            tokens.push_back(text.substr(begin, i - begin));
        }
    }
    return tokens;
}

} // namespace

bool is_font_variant_alternates_word(std::string_view word) {
    if (word == "historical-forms") {
        return true;
    }
    const std::size_t open = word.find('(');
    return open != std::string_view::npos && function_named(word.substr(0, open)).has_value();
}

std::optional<std::vector<FontVariantAlternateSetting>>
parse_font_variant_alternates(std::string_view value) {
    const std::string text = tessera::detail::lower(tessera::detail::trim(value));
    if (text.empty()) {
        return std::nullopt;
    }
    std::vector<FontVariantAlternateSetting> out;
    if (text == "normal") {
        return out;
    }
    const auto tokens = split_tokens(text);
    if (!tokens || tokens->empty()) {
        return std::nullopt;
    }
    std::array<bool, static_cast<std::size_t>(Function::kCount)> seen{};
    for (const std::string_view token : *tokens) {
        Function function = Function::Historical;
        std::vector<Argument> arguments;
        if (token != "historical-forms") {
            const std::size_t open = token.find('(');
            if (open == std::string_view::npos || token.back() != ')') {
                return std::nullopt;
            }
            const auto named = function_named(token.substr(0, open));
            const auto parsed = parse_arguments(token.substr(open + 1, token.size() - open - 2));
            if (!named || !parsed) {
                return std::nullopt;
            }
            function = *named;
            arguments = *parsed;
        }
        bool& was_seen = seen[static_cast<std::size_t>(function)];
        if (was_seen) {
            return std::nullopt;
        }
        was_seen = true;
        switch (function) {
        case Function::Historical:
            add(out, "hist", 1);
            break;
        case Function::Stylistic:
        case Function::Swash:
        case Function::Ornaments:
        case Function::Annotation: {
            // One index only; a list is invalid for these.
            if (arguments.size() != 1) {
                return std::nullopt;
            }
            if (!arguments[0].has_index) {
                break;
            }
            const std::uint32_t index = arguments[0].index;
            if (function == Function::Stylistic) {
                add(out, "salt", index);
            } else if (function == Function::Swash) {
                add(out, "swsh", index);
                add(out, "cswh", index);
            } else if (function == Function::Ornaments) {
                add(out, "ornm", index);
            } else {
                add(out, "nalt", index);
            }
            break;
        }
        case Function::Styleset:
            for (const Argument& argument : arguments) {
                if (argument.has_index && argument.index >= 1 && argument.index <= 20) {
                    add_numbered(out, 's', 's', argument.index);
                }
            }
            break;
        case Function::CharacterVariant:
            for (const Argument& argument : arguments) {
                if (argument.has_index && argument.index >= 1 && argument.index <= 99) {
                    add_numbered(out, 'c', 'v', argument.index);
                }
            }
            break;
        case Function::kCount:
            break;
        }
    }
    return out;
}

} // namespace tessera::ui::style
