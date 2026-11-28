// SPDX-License-Identifier: MIT

#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/font_variant.hpp"

#include <array>
#include <iterator>
#include <string>

namespace tessera::ui::style {
namespace {

using G = FontVariantGroup;
constexpr std::uint8_t kAlone = 255;

constexpr FontVariantKeyword kKeywords[] = {
    {"none",
     G::Ligatures,
     kAlone,
     5,
     {{{"liga", 0}, {"clig", 0}, {"dlig", 0}, {"hlig", 0}, {"calt", 0}}}},
    {"common-ligatures", G::Ligatures, 1, 1, {{{"liga", 1}}}},
    {"no-common-ligatures", G::Ligatures, 1, 2, {{{"liga", 0}, {"clig", 0}}}},
    {"discretionary-ligatures", G::Ligatures, 2, 1, {{{"dlig", 1}}}},
    {"no-discretionary-ligatures", G::Ligatures, 2, 1, {{{"dlig", 0}}}},
    {"historical-ligatures", G::Ligatures, 3, 1, {{{"hlig", 1}}}},
    {"no-historical-ligatures", G::Ligatures, 3, 1, {{{"hlig", 0}}}},
    {"contextual", G::Ligatures, 4, 1, {{{"calt", 1}}}},
    {"no-contextual", G::Ligatures, 4, 1, {{{"calt", 0}}}},

    {"lining-nums", G::Numeric, 1, 1, {{{"lnum", 1}}}},
    {"oldstyle-nums", G::Numeric, 1, 1, {{{"onum", 1}}}},
    {"proportional-nums", G::Numeric, 2, 1, {{{"pnum", 1}}}},
    {"tabular-nums", G::Numeric, 2, 1, {{{"tnum", 1}}}},
    {"diagonal-fractions", G::Numeric, 3, 1, {{{"frac", 1}}}},
    {"stacked-fractions", G::Numeric, 3, 1, {{{"afrc", 1}}}},
    {"ordinal", G::Numeric, 4, 1, {{{"ordn", 1}}}},
    {"slashed-zero", G::Numeric, 5, 1, {{{"zero", 1}}}},

    {"small-caps", G::Caps, 1, 1, {{{"smcp", 1}}}},
    {"all-small-caps", G::Caps, 1, 2, {{{"smcp", 1}, {"c2sc", 1}}}},
    {"petite-caps", G::Caps, 1, 1, {{{"pcap", 1}}}},
    {"all-petite-caps", G::Caps, 1, 2, {{{"pcap", 1}, {"c2pc", 1}}}},
    {"unicase", G::Caps, 1, 1, {{{"unic", 1}}}},
    {"titling-caps", G::Caps, 1, 1, {{{"titl", 1}}}},

    {"jis78", G::EastAsian, 1, 1, {{{"jp78", 1}}}},
    {"jis83", G::EastAsian, 1, 1, {{{"jp83", 1}}}},
    {"jis90", G::EastAsian, 1, 1, {{{"jp90", 1}}}},
    {"jis04", G::EastAsian, 1, 1, {{{"jp04", 1}}}},
    {"simplified", G::EastAsian, 1, 1, {{{"smpl", 1}}}},
    {"traditional", G::EastAsian, 1, 1, {{{"trad", 1}}}},
    {"full-width", G::EastAsian, 2, 1, {{{"fwid", 1}}}},
    {"proportional-width", G::EastAsian, 2, 1, {{{"pwid", 1}}}},
    {"ruby", G::EastAsian, 3, 1, {{{"ruby", 1}}}},

    {"sub", G::Position, 1, 1, {{{"subs", 1}}}},
    {"super", G::Position, 1, 1, {{{"sups", 1}}}},
};

} // namespace

const FontVariantKeyword* find_font_variant_keyword(std::string_view lowered) {
    for (const FontVariantKeyword& keyword : kKeywords) {
        if (keyword.name == lowered) {
            return &keyword;
        }
    }
    return nullptr;
}

std::string_view font_variant_property(FontVariantGroup group) noexcept {
    switch (group) {
    case G::Ligatures:
        return "font-variant-ligatures";
    case G::Numeric:
        return "font-variant-numeric";
    case G::Caps:
        return "font-variant-caps";
    case G::EastAsian:
        return "font-variant-east-asian";
    case G::Position:
        return "font-variant-position";
    }
    return {};
}

std::optional<FontVariantParsed> parse_font_variant_value(FontVariantGroup group,
                                                          std::string_view value) {
    const std::string text = tessera::detail::lower(tessera::detail::trim(value));
    if (text.empty()) {
        return std::nullopt;
    }
    FontVariantParsed parsed;
    if (text == "normal") {
        return parsed;
    }
    // Bit per exclusive class (classes are small); a repeat is a conflict.
    std::uint32_t seen = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n')) {
            ++i;
        }
        const std::size_t begin = i;
        while (i < text.size() && text[i] != ' ' && text[i] != '\t' && text[i] != '\n') {
            ++i;
        }
        if (i == begin) {
            break;
        }
        const FontVariantKeyword* keyword =
            find_font_variant_keyword(std::string_view(text).substr(begin, i - begin));
        if (keyword == nullptr || keyword->group != group || parsed.count >= kMaxFontVariantWords) {
            return std::nullopt;
        }
        if (keyword->exclusive != 0) {
            if (keyword->exclusive == kAlone) {
                if (parsed.count != 0) {
                    return std::nullopt;
                }
                seen |= 1U << 31U;
            } else {
                const std::uint32_t bit = 1U << keyword->exclusive;
                if ((seen & bit) != 0 || (seen & (1U << 31U)) != 0) {
                    return std::nullopt;
                }
                seen |= bit;
            }
        } else if ((seen & (1U << 31U)) != 0) {
            return std::nullopt;
        }
        parsed.items[parsed.count++] = keyword;
    }
    if (parsed.count == 0) {
        return std::nullopt;
    }
    if ((seen & (1U << 31U)) != 0 && parsed.count != 1) {
        return std::nullopt;
    }
    return parsed;
}

} // namespace tessera::ui::style
