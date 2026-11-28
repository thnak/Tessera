// SPDX-License-Identifier: MIT

#include "property_font_variant.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/font_variant.hpp"
#include "tessera/ui/style/font_variant_alternates.hpp"

#include <array>
#include <string>
#include <utility>

namespace tessera::ui::style {

PropertyExpansionResult expand_font_variant_shorthand(const Declaration& declaration) {
    PropertyExpansionResult result;
    const auto reject = [&](std::string message) {
        result.declarations.clear();
        result.diagnostics.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue,
                                                     declaration.offset, "font-variant",
                                                     std::move(message)});
        return result;
    };

    const std::string text = tessera::detail::lower(tessera::detail::trim(declaration.value));
    if (text.empty()) {
        return reject("font-variant needs normal, none or font-variant keywords");
    }
    constexpr std::size_t kGroups = 5;
    // Slot kGroups is font-variant-alternates, whose functional words keep their inner spaces.
    std::array<std::string, kGroups + 1> values{};
    if (text == "none") {
        values[static_cast<std::size_t>(FontVariantGroup::Ligatures)] = "none";
    } else if (text != "normal") {
        std::size_t i = 0;
        while (i < text.size()) {
            while (i < text.size() && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n')) {
                ++i;
            }
            const std::size_t begin = i;
            int depth = 0;
            while (i < text.size() &&
                   (depth > 0 || (text[i] != ' ' && text[i] != '\t' && text[i] != '\n'))) {
                depth += text[i] == '(' ? 1 : text[i] == ')' ? -1 : 0;
                ++i;
            }
            if (i == begin) {
                break;
            }
            const std::string word = text.substr(begin, i - begin);
            if (is_font_variant_alternates_word(word)) {
                std::string& slot = values[kGroups];
                slot += slot.empty() ? word : " " + word;
                continue;
            }
            const FontVariantKeyword* keyword = find_font_variant_keyword(word);
            if (keyword == nullptr || word == "none") {
                return reject("unsupported font-variant component `" + word + "`");
            }
            std::string& slot = values[static_cast<std::size_t>(keyword->group)];
            slot += slot.empty() ? word : " " + word;
        }
    }
    for (std::size_t g = 0; g < kGroups; ++g) {
        const auto group = static_cast<FontVariantGroup>(g);
        if (values[g].empty()) {
            values[g] = "normal";
        } else if (!parse_font_variant_value(group, values[g])) {
            return reject("font-variant has conflicting or repeated keywords for " +
                          std::string(font_variant_property(group)));
        }
    }
    if (values[kGroups].empty()) {
        values[kGroups] = "normal";
    } else if (!parse_font_variant_alternates(values[kGroups])) {
        return reject("font-variant has conflicting or malformed alternates components");
    }
    for (std::size_t g = 0; g < kGroups; ++g) {
        result.declarations.push_back(
            Declaration{std::string(font_variant_property(static_cast<FontVariantGroup>(g))),
                        std::move(values[g]), declaration.important, declaration.offset});
    }
    result.declarations.push_back(Declaration{std::string(kFontVariantAlternatesProperty),
                                              std::move(values[kGroups]), declaration.important,
                                              declaration.offset});
    return result;
}

} // namespace tessera::ui::style
