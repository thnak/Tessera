// SPDX-License-Identifier: MIT

#include "property_list_style.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/list_style.hpp"

#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace tessera::ui::style {

using tessera::detail::lower;
namespace {

std::vector<std::string_view> components_of(std::string_view text) {
    std::vector<std::string_view> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        const bool end = i == text.size();
        if (!end && text[i] == '(') {
            ++depth;
        } else if (!end && text[i] == ')') {
            --depth;
        }
        if (end || (depth == 0 && std::isspace(static_cast<unsigned char>(text[i])) != 0)) {
            if (i > start) {
                parts.push_back(text.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    return parts;
}

} // namespace

PropertyExpansionResult expand_list_style_shorthand(const Declaration& declaration) {
    PropertyExpansionResult result;
    const auto reject = [&](std::string message) {
        result.declarations.clear();
        result.diagnostics.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue,
                                                     declaration.offset, "list-style",
                                                     std::move(message)});
        return result;
    };

    const std::vector<std::string_view> parts = components_of(declaration.value);
    if (parts.empty()) {
        return reject("list-style needs a marker type or none");
    }
    std::string type = "disc";
    std::string position = "outside";
    bool have_type = false;
    bool have_position = false;
    for (const std::string_view part : parts) {
        const std::string word = lower(part);
        if (word.starts_with("url(")) {
            return reject("list-style-image is not supported; draw the marker with a type");
        }
        if (word == "outside" || word == "inside") {
            if (have_position) {
                return reject("list-style has more than one position");
            }
            position = word;
            have_position = true;
        } else if (parse_list_style_type(word).has_value()) {
            if (have_type) {
                return reject("list-style has more than one marker type");
            }
            type = word;
            have_type = true;
        } else {
            return reject("unsupported list-style component `" + std::string(part) + "`");
        }
    }
    const auto emit = [&](const char* property, std::string value) {
        result.declarations.push_back(
            Declaration{property, std::move(value), declaration.important, declaration.offset});
    };
    emit("list-style-type", std::move(type));
    emit("list-style-position", std::move(position));
    return result;
}

} // namespace tessera::ui::style
