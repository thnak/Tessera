// SPDX-License-Identifier: MIT

#include "property_background.hpp"
#include "property_mask.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/style/background_layers.hpp"
#include "tessera/ui/style/gradient.hpp"
#include "tessera/ui/style/mask_layers.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tessera::ui::style {

using tessera::detail::lower;
namespace {

bool is_mode(const std::string& word) {
    return word == "alpha" || word == "luminance" || word == "match-source";
}

bool is_repeat(const std::string& word) {
    return word == "repeat-x" || word == "repeat-y" || word == "repeat" || word == "space" ||
           word == "round" || word == "no-repeat";
}

bool is_unsupported_part(const std::string& word) {
    return word == "margin-box" || word == "add" || word == "subtract" || word == "intersect" ||
           word == "exclude";
}

bool is_box(const std::string& word) {
    return word == "border-box" || word == "padding-box" || word == "content-box" ||
           word == "fill-box" || word == "stroke-box" || word == "view-box";
}

// The slash separates the position from the size; it is spaced out (outside parentheses, so the
// slash in rgb(0 0 0 / .5) stays) so the component split sees it as its own word.
std::string space_slashes(std::string_view text) {
    std::string out;
    int depth = 0;
    for (const char c : text) {
        if (c == '(') {
            ++depth;
        } else if (c == ')') {
            --depth;
        }
        if (c == '/' && depth == 0) {
            out += " / ";
        } else {
            out += c;
        }
    }
    return out;
}

struct Layer {
    std::string image = "none";
    std::string position;
    std::string size;
    std::string repeat;
    std::string mode;
    std::string origin;
    std::string clip;
};

void append(std::string& list, const std::string& value, std::size_t index) {
    if (index != 0) {
        list += ", ";
    }
    list += value;
}

void join(std::string& list, const std::string& word) {
    list += list.empty() ? word : " " + word;
}

} // namespace

PropertyExpansionResult expand_mask_shorthand(const Declaration& declaration) {
    PropertyExpansionResult result;
    const auto reject = [&](std::string message) {
        result.declarations.clear();
        result.diagnostics.push_back(StyleDiagnostic{
            StyleDiagnosticKind::UnsupportedValue, declaration.offset, "mask", std::move(message)});
        return result;
    };

    const std::vector<std::string_view> texts = tessera::detail::split_commas(declaration.value);
    if (texts.empty() || texts.size() > kMaxBackgroundLayers) {
        return reject("mask takes 1 to 8 comma-separated layers");
    }
    std::vector<Layer> layers;
    for (const std::string_view text : texts) {
        if (text.empty()) {
            return reject("mask has an empty layer");
        }
        Layer layer;
        bool have_image = false;
        bool have_mode = false;
        bool after_slash = false;
        int repeat_words = 0;
        int box_words = 0;
        bool no_clip = false;
        // The components view into this string, so it must outlive the loop.
        const std::string spaced = space_slashes(text);
        for (const std::string_view component : split_layer_components(spaced)) {
            const std::string word = lower(component);
            if (is_gradient_function(component) || word == "none") {
                if (have_image) {
                    return reject("a mask layer carries at most one image");
                }
                if (word != "none" && !parse_gradient(component)) {
                    return reject("unsupported gradient in mask; only linear-gradient(), "
                                  "radial-gradient() or conic-gradient() (optionally repeating-) "
                                  "with 2 to 8 colour stops is supported");
                }
                have_image = true;
                layer.image = std::string(component);
            } else if (word == "/") {
                if (after_slash || layer.position.empty()) {
                    return reject("a mask layer's / needs a position before it and a size after");
                }
                after_slash = true;
            } else if (is_mode(word)) {
                if (have_mode) {
                    return reject("a mask layer carries at most one mode");
                }
                have_mode = true;
                layer.mode = word;
            } else if (is_repeat(word)) {
                const bool axis_word = word == "repeat-x" || word == "repeat-y";
                if (repeat_words == 2 || (repeat_words == 1 && axis_word) ||
                    layer.repeat == "repeat-x" || layer.repeat == "repeat-y") {
                    return reject("a mask layer carries one repeat style");
                }
                ++repeat_words;
                join(layer.repeat, word);
            } else if (is_box(word)) {
                // One box sets origin and clip; a second is the clip (CSS Masking 1 section 6.4).
                if (++box_words > 2) {
                    return reject("a mask layer carries at most two geometry boxes");
                }
                (box_words == 1 ? layer.origin : layer.clip) = word;
            } else if (word == "no-clip") {
                if (no_clip) {
                    return reject("a mask layer carries at most one no-clip");
                }
                no_clip = true;
            } else if (is_unsupported_part(word)) {
                return reject("mask-composite and margin-box are not supported; '" + word +
                              "' is ignored with the whole declaration");
            } else if (after_slash) {
                join(layer.size, word);
            } else {
                join(layer.position, word);
            }
        }
        if (no_clip && box_words == 2) {
            return reject("no-clip takes at most one geometry box (the origin)");
        }
        if (box_words == 1 && !no_clip) {
            layer.clip = layer.origin;
        }
        if (no_clip) {
            layer.clip = "no-clip";
        }
        if (after_slash && layer.size.empty()) {
            return reject("a mask layer's / needs a size after it");
        }
        if ((!layer.position.empty() && !parse_mask_position(layer.position)) ||
            (!layer.size.empty() && !parse_mask_size(layer.size)) ||
            (!layer.repeat.empty() && !parse_mask_repeat(layer.repeat))) {
            return reject("unsupported mask layer '" + std::string(text) +
                          "'; position takes 1 to 4 keyword/px/% values, size takes auto, cover, "
                          "contain or 1 to 2 px/% values");
        }
        layers.push_back(std::move(layer));
    }

    std::string image;
    std::string position;
    std::string size;
    std::string repeat;
    std::string mode;
    std::string origin;
    std::string clip;
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const Layer& layer = layers[index];
        append(image, layer.image, index);
        append(position, layer.position.empty() ? "0% 0%" : layer.position, index);
        append(size, layer.size.empty() ? "auto" : layer.size, index);
        append(repeat, layer.repeat.empty() ? "repeat" : layer.repeat, index);
        append(mode, layer.mode.empty() ? "match-source" : layer.mode, index);
        append(origin, layer.origin.empty() ? "border-box" : layer.origin, index);
        append(clip, layer.clip.empty() ? "border-box" : layer.clip, index);
    }
    const auto emit = [&](const char* property, std::string value) {
        result.declarations.push_back(
            Declaration{property, std::move(value), declaration.important, declaration.offset});
    };
    emit("mask-image", std::move(image));
    emit("mask-position", std::move(position));
    emit("mask-size", std::move(size));
    emit("mask-repeat", std::move(repeat));
    emit("mask-mode", std::move(mode));
    emit("mask-origin", std::move(origin));
    emit("mask-clip", std::move(clip));
    return result;
}

} // namespace tessera::ui::style
