// SPDX-License-Identifier: MIT

#include "tessera/ui/style/safe_area_style.hpp"

#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace tessera::ui::style {

namespace {

// Calls `fn(token)` for every non-empty `separator`-separated token of `text`.
template<typename Fn> void for_each_token(std::string_view text, char separator, Fn&& fn) {
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && text[index] == separator) {
            ++index;
        }
        const std::size_t start = index;
        while (index < text.size() && text[index] != separator) {
            ++index;
        }
        if (index > start) {
            fn(text.substr(start, index - start));
        }
    }
}

float parse_px(std::string_view text) noexcept {
    char buffer[32];
    if (text.empty() || text.size() >= sizeof buffer) {
        return 0.0f;
    }
    text.copy(buffer, text.size());
    buffer[text.size()] = '\0';
    char* end = nullptr;
    const float value = std::strtof(buffer, &end);
    return end != buffer && std::isfinite(value) && value > 0.0f ? value : 0.0f;
}

bool starts_with(std::string_view text, std::string_view prefix) noexcept {
    return text.substr(0, prefix.size()) == prefix;
}

std::uint8_t edge_bit(std::string_view word) noexcept {
    if (word == "top") {
        return hal::kEdgeTop;
    }
    if (word == "right") {
        return hal::kEdgeRight;
    }
    if (word == "bottom") {
        return hal::kEdgeBottom;
    }
    if (word == "left") {
        return hal::kEdgeLeft;
    }
    if (word == "start") {
        return hal::kEdgeStart;
    }
    return word == "end" ? hal::kEdgeEnd : 0;
}

std::uint8_t source_bit(std::string_view word) noexcept {
    if (word == "system-bars") {
        return hal::kSourceSystemBars;
    }
    if (word == "cutout") {
        return hal::kSourceCutout;
    }
    if (word == "ime") {
        return hal::kSourceIme;
    }
    if (word == "gestures") {
        return hal::kSourceGestures;
    }
    return word == "hinge" ? hal::kSourceHinge : 0;
}

// The `direction` a node resolved to, as the `rtl` flag the arithmetic takes. An absent or
// non-`rtl` value is left-to-right, matching the cascade's `direction: ltr` initial value.
bool is_rtl(const ComputedStyle& style) noexcept {
    const std::string* direction = style.find("direction");
    return direction != nullptr && *direction == "rtl";
}

} // namespace

bool is_safe_area_node(const ui::Node& node) {
    bool found = false;
    for_each_token(node.class_name(), ' ',
                   [&](std::string_view token) { found = found || token == "tsafe"; });
    return found;
}

std::optional<SafeAreaSpec> read_safe_area_spec(const ui::Node& node, bool rtl) {
    if (!is_safe_area_node(node)) {
        return std::nullopt;
    }
    SafeAreaSpec spec;
    spec.config.rtl = rtl;
    for_each_token(node.class_name(), ' ', [&](std::string_view token) {
        if (starts_with(token, "safe-edges-")) {
            spec.config.edges = 0;
            for_each_token(token.substr(11), '+', [&](std::string_view word) {
                spec.config.edges = static_cast<std::uint8_t>(spec.config.edges | edge_bit(word));
            });
        } else if (starts_with(token, "safe-sources-")) {
            spec.config.sources = 0;
            for_each_token(token.substr(13), '+', [&](std::string_view word) {
                spec.config.sources =
                    static_cast<std::uint8_t>(spec.config.sources | source_bit(word));
            });
        } else if (starts_with(token, "safe-min-")) {
            spec.config.minimum = parse_px(token.substr(9));
        } else if (token == "safe-mode-margin") {
            spec.margin = true;
        }
    });
    return spec;
}

void apply_safe_area(ComputedStyle& style, const SafeAreaSpec& spec,
                     const hal::WindowInsets& insets) {
    const char* prefix = spec.margin ? "margin-" : "padding-";
    const char* const sides[] = {"top", "right", "bottom", "left"};
    const float values[] = {insets.top, insets.right, insets.bottom, insets.left};
    for (std::size_t i = 0; i < 4; ++i) {
        char text[32];
        std::snprintf(text, sizeof text, "%gpx", static_cast<double>(values[i]));
        style.set(std::string(prefix) + sides[i], text);
    }
    style.erase(std::string(prefix) + "inline-start");
    style.erase(std::string(prefix) + "inline-end");
}

bool apply_safe_area_style(ComputedStyle& style, const ui::Node& node,
                           const hal::WindowMetrics* metrics) {
    if (metrics == nullptr || !is_safe_area_node(node)) {
        return false;
    }
    const std::optional<SafeAreaSpec> spec = read_safe_area_spec(node, is_rtl(style));
    if (!spec) {
        return false;
    }
    apply_safe_area(style, *spec, hal::resolve_safe_area(*metrics, spec->config));
    return true;
}

} // namespace tessera::ui::style
