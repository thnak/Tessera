// SPDX-License-Identifier: MIT

#include "property_background.hpp"
#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/gradient.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

// Splits at whitespace outside parentheses, so `rgba(0, 0, 0, .2)` and a whole `linear-gradient()`
// stay one component.
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

std::string lower(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

} // namespace

bool is_window_backdrop_value(std::string_view text) {
    const std::string value = lower(text);
    return value == "none" || value == "auto" || value == "mica" || value == "acrylic" ||
           value == "blur";
}

PropertyExpansionResult expand_background_shorthand(const Declaration& declaration) {
    PropertyExpansionResult result;
    const auto reject = [&](std::string message) {
        result.declarations.clear();
        result.diagnostics.push_back(StyleDiagnostic{StyleDiagnosticKind::UnsupportedValue,
                                                     declaration.offset, "background",
                                                     std::move(message)});
        return result;
    };
    const auto emit = [&](const char* property, std::string value) {
        result.declarations.push_back(
            Declaration{property, std::move(value), declaration.important, declaration.offset});
    };

    const std::vector<std::string_view> parts = components_of(declaration.value);
    if (parts.empty()) {
        return reject("background needs a colour, a linear-gradient() or none");
    }
    if (parts.size() == 1 && lower(parts[0]) == "none") {
        emit("background-color", "transparent");
        emit("background-image", "none");
        return result;
    }
    bool have_color = false;
    bool have_image = false;
    for (const std::string_view part : parts) {
        if (lower(part).rfind("linear-gradient(", 0) == 0) {
            if (have_image || !parse_linear_gradient(part)) {
                return reject("unsupported gradient in background; only one linear-gradient() with "
                              "2 to 8 colour stops is supported");
            }
            have_image = true;
            emit("background-image", std::string(part));
        } else if (!have_color && parse_color(part).ok) {
            have_color = true;
            emit("background-color", std::string(part));
        } else {
            return reject("unsupported background layer `" + std::string(part) +
                          "`; only a colour and one linear-gradient() are supported");
        }
    }
    // A shorthand resets the longhands it does not name.
    if (!have_color) {
        emit("background-color", "transparent");
    }
    if (!have_image) {
        emit("background-image", "none");
    }
    return result;
}

} // namespace tessera::ui::style
