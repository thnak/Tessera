// SPDX-License-Identifier: MIT

#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/list_style.hpp"

#include <array>
#include <cstddef>
#include <utility>

namespace tessera::ui::style {
namespace {

using tessera::detail::lower;
using tessera::detail::trim;

constexpr std::array<std::pair<std::string_view, ListStyleType>, 11> kTypes{{
    {"none", ListStyleType::None},
    {"disc", ListStyleType::Disc},
    {"circle", ListStyleType::Circle},
    {"square", ListStyleType::Square},
    {"decimal", ListStyleType::Decimal},
    {"lower-alpha", ListStyleType::LowerAlpha},
    {"lower-latin", ListStyleType::LowerAlpha},
    {"upper-alpha", ListStyleType::UpperAlpha},
    {"upper-latin", ListStyleType::UpperAlpha},
    {"lower-roman", ListStyleType::LowerRoman},
    {"upper-roman", ListStyleType::UpperRoman},
}};

// Bijective base 26: 1 is a, 26 is z, 27 is aa.
std::string alphabetic(std::uint32_t ordinal, char first) {
    std::string text;
    while (ordinal > 0) {
        --ordinal;
        text.insert(text.begin(), static_cast<char>(first + static_cast<char>(ordinal % 26)));
        ordinal /= 26;
    }
    return text;
}

std::string roman(std::uint32_t ordinal, bool upper) {
    struct Step {
        std::uint32_t value;
        const char* lower_text;
        const char* upper_text;
    };
    constexpr Step kSteps[] = {
        {1000, "m", "M"}, {900, "cm", "CM"}, {500, "d", "D"},  {400, "cd", "CD"}, {100, "c", "C"},
        {90, "xc", "XC"}, {50, "l", "L"},    {40, "xl", "XL"}, {10, "x", "X"},    {9, "ix", "IX"},
        {5, "v", "V"},    {4, "iv", "IV"},   {1, "i", "I"}};
    std::string text;
    for (const Step& step : kSteps) {
        while (ordinal >= step.value) {
            text += upper ? step.upper_text : step.lower_text;
            ordinal -= step.value;
        }
    }
    return text;
}

} // namespace

std::optional<ListStyleType> parse_list_style_type(std::string_view value) {
    const std::string word = lower(trim(value));
    for (const auto& [name, type] : kTypes) {
        if (word == name) {
            return type;
        }
    }
    return std::nullopt;
}

bool is_supported_list_style_position(std::string_view value) {
    return lower(trim(value)) == "outside";
}

bool is_bullet_marker(ListStyleType type) {
    return type == ListStyleType::Disc || type == ListStyleType::Circle ||
           type == ListStyleType::Square;
}

std::string list_marker_text(ListStyleType type, std::uint32_t ordinal) {
    std::string counter;
    switch (type) {
    case ListStyleType::None:
    case ListStyleType::Disc:
    case ListStyleType::Circle:
    case ListStyleType::Square:
        return {};
    case ListStyleType::Decimal:
        counter = std::to_string(ordinal);
        break;
    case ListStyleType::LowerAlpha:
        counter = ordinal == 0 ? std::to_string(ordinal) : alphabetic(ordinal, 'a');
        break;
    case ListStyleType::UpperAlpha:
        counter = ordinal == 0 ? std::to_string(ordinal) : alphabetic(ordinal, 'A');
        break;
    case ListStyleType::LowerRoman:
        counter = ordinal == 0 || ordinal > 3999 ? std::to_string(ordinal) : roman(ordinal, false);
        break;
    case ListStyleType::UpperRoman:
        counter = ordinal == 0 || ordinal > 3999 ? std::to_string(ordinal) : roman(ordinal, true);
        break;
    }
    return counter + ". ";
}

} // namespace tessera::ui::style
