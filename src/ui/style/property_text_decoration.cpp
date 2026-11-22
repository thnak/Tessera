// SPDX-License-Identifier: MIT

#include "property_text_decoration.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/text_decoration.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace tessera::ui::style {

using tessera::detail::lower;
namespace {

// Whitespace split outside parentheses, so `rgba(0, 0, 0, .5)` stays one component.
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

PropertyExpansionResult expand_text_decoration_shorthand(const Declaration& declaration) {
    PropertyExpansionResult result;
    const auto reject = [&](std::string message) {
        result.declarations.clear();
        result.diagnostics.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue,
                                                     declaration.offset, "text-decoration",
                                                     std::move(message)});
        return result;
    };

    const std::vector<std::string_view> parts = components_of(declaration.value);
    if (parts.empty()) {
        return reject("text-decoration needs a line, a colour, a thickness or none");
    }
    std::string line_text;
    std::string color = "currentColor";
    std::string thickness = "auto";
    bool have_line = false;
    bool have_color = false;
    bool have_thickness = false;
    for (const std::string_view part : parts) {
        const std::string word = lower(part);
        if (word == "underline" || word == "overline" || word == "line-through" || word == "none") {
            // The line keywords combine (`underline line-through`) and `none` stands alone;
            // parse_text_decoration_line judges the joined list, so both rules live in one place.
            line_text += line_text.empty() ? word : " " + word;
            have_line = true;
        } else if (word == "double" || word == "dotted" || word == "dashed" || word == "wavy") {
            return reject("only solid text-decoration-style is supported; `" + word +
                          "` is not drawn");
        } else if (word == "solid") {
            continue;
        } else if (is_supported_text_decoration_thickness(word)) {
            if (have_thickness) {
                return reject("text-decoration has more than one thickness");
            }
            thickness = word;
            have_thickness = true;
        } else if (word == "currentcolor" || parse_color(part).ok) {
            if (have_color) {
                return reject("text-decoration has more than one colour");
            }
            color = std::string(part);
            have_color = true;
        } else {
            return reject("unsupported text-decoration component `" + std::string(part) + "`");
        }
    }
    if (have_line && !parse_text_decoration_line(line_text)) {
        return reject("text-decoration line must be none or a set of underline, overline and "
                      "line-through without repeats");
    }
    const auto emit = [&](const char* property, std::string value) {
        result.declarations.push_back(
            Declaration{property, std::move(value), declaration.important, declaration.offset});
    };
    emit("text-decoration-line", have_line ? line_text : "none");
    emit("text-decoration-style", "solid");
    emit("text-decoration-color", std::move(color));
    emit("text-decoration-thickness", std::move(thickness));
    return result;
}

} // namespace tessera::ui::style
