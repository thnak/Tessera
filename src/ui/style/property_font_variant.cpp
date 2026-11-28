// SPDX-License-Identifier: MIT

#include "property_font_variant.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/font_variant.hpp"

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
    std::array<std::string, kGroups> values{};
    if (text == "none") {
        values[static_cast<std::size_t>(FontVariantGroup::Ligatures)] = "none";
    } else if (text != "normal") {
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
            const std::string word = text.substr(begin, i - begin);
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
    for (std::size_t g = 0; g < kGroups; ++g) {
        result.declarations.push_back(
            Declaration{std::string(font_variant_property(static_cast<FontVariantGroup>(g))),
                        std::move(values[g]), declaration.important, declaration.offset});
    }
    return result;
}

} // namespace tessera::ui::style
