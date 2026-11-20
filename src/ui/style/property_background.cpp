// SPDX-License-Identifier: MIT

#include "property_background.hpp"
#include "tessera/ui/style/background_layers.hpp"
#include "tessera/ui/style/color.hpp"
#include "tessera/ui/style/gradient.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {
namespace {

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    return text.substr(first, text.find_last_not_of(" \t\r\n\f") - first + 1);
}

// Splits at top-level commas, so the commas inside `rgba(0, 0, 0, .2)` and inside a gradient's stop
// list stay part of their layer.
std::vector<std::string_view> layers_of(std::string_view text) {
    std::vector<std::string_view> parts;
    int depth = 0;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= text.size(); ++i) {
        if (i < text.size()) {
            if (text[i] == '(') {
                ++depth;
            } else if (text[i] == ')') {
                --depth;
            }
        }
        if (i == text.size() || (text[i] == ',' && depth == 0)) {
            const std::string_view part = trim(text.substr(start, i - start));
            if (!part.empty()) {
                parts.push_back(part);
            }
            start = i + 1;
        }
    }
    return parts;
}

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

    // CSS Backgrounds 3 section 2.1: `background` is `<bg-layer># , <final-bg-layer>`, where a
    // layer is an image and/or a position/size/repeat/attachment/origin/clip, and only the final
    // layer may carry the colour. V1 supports the image part of a layer (a gradient) and the colour
    // on the final layer; every other component is reported rather than half applied.
    const std::vector<std::string_view> layers = layers_of(declaration.value);
    if (layers.empty()) {
        return reject("background needs a colour, a gradient or none");
    }
    if (layers.size() == 1 && lower(layers[0]) == "none") {
        emit("background-color", "transparent");
        emit("background-image", "none");
        return result;
    }

    std::vector<std::string_view> images;
    images.reserve(layers.size());
    bool have_color = false;
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const bool final_layer = index + 1 == layers.size();
        std::string_view image;
        for (const std::string_view component : components_of(layers[index])) {
            if (is_gradient_function(component)) {
                if (!image.empty()) {
                    return reject("a background layer carries at most one image");
                }
                if (!parse_gradient(component)) {
                    return reject("unsupported gradient in background; only linear-gradient() or "
                                  "radial-gradient() (optionally repeating-) with 2 to 8 colour "
                                  "stops is supported");
                }
                image = component;
                continue;
            }
            // The colour is only legal on the final layer, as in CSS.
            if (final_layer && !have_color && parse_color(component).ok) {
                have_color = true;
                emit("background-color", std::string(component));
                continue;
            }
            return reject("unsupported background layer component `" + std::string(component) +
                          "`; only a colour on the final layer and gradient layers are supported");
        }
        images.push_back(image);
    }
    if (images.size() > kMaxBackgroundLayers) {
        return reject("too many background layers; at most 8 are supported");
    }

    // The longhand keeps the CSS order (first layer on top), so the paint pass can walk it
    // backwards and the interpolator can pair it positionally.
    std::string list;
    for (std::size_t index = 0; index < images.size(); ++index) {
        if (index != 0) {
            list += ", ";
        }
        list += images[index].empty() ? std::string_view("none") : images[index];
    }
    emit("background-image", std::move(list));

    // A shorthand resets the longhands it does not name.
    if (!have_color) {
        emit("background-color", "transparent");
    }
    return result;
}

} // namespace tessera::ui::style
