// SPDX-License-Identifier: MIT

#include "safe_area_state.hpp"

#include "tessera/ui/style/cascade.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace tessera::app {

namespace {

constexpr float kSnapDistance = 0.05f;

bool ease(float& value, float target, double factor) noexcept {
    if (value == target) {
        return false;
    }
    const float delta = target - value;
    if (std::fabs(delta) <= kSnapDistance) {
        value = target;
        return true;
    }
    value += delta * static_cast<float>(factor);
    return true;
}

bool ease(hal::WindowInsets& value, const hal::WindowInsets& target, double factor) noexcept {
    bool changed = false;
    changed |= ease(value.left, target.left, factor);
    changed |= ease(value.top, target.top, factor);
    changed |= ease(value.right, target.right, factor);
    changed |= ease(value.bottom, target.bottom, factor);
    return changed;
}

bool settled(const hal::WindowInsets& a, const hal::WindowInsets& b) noexcept {
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

bool same_rect(const hal::WindowRect& a, const hal::WindowRect& b) noexcept {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

// True when every input of `resolve_safe_area` is equal.
bool same_inputs(const hal::WindowMetrics& a, const hal::WindowMetrics& b) noexcept {
    if (a.logical_width != b.logical_width || a.logical_height != b.logical_height ||
        !settled(a.insets, b.insets) || !settled(a.ime_insets, b.ime_insets) ||
        !settled(a.gesture_insets, b.gesture_insets) ||
        !settled(a.cutouts.waterfall_insets, b.cutouts.waterfall_insets) ||
        a.cutouts.count != b.cutouts.count || a.fold.occlusion != b.fold.occlusion ||
        a.fold.orientation != b.fold.orientation ||
        !same_rect(a.fold.hinge_bounds, b.fold.hinge_bounds)) {
        return false;
    }
    for (std::size_t i = 0; i < a.cutouts.count && i < hal::kMaxCutouts; ++i) {
        if (!same_rect(a.cutouts.bounds[i], b.cutouts.bounds[i])) {
            return false;
        }
    }
    return true;
}

bool animated_settled(const hal::WindowMetrics& a, const hal::WindowMetrics& b) noexcept {
    return settled(a.insets, b.insets) && settled(a.ime_insets, b.ime_insets) &&
           settled(a.gesture_insets, b.gesture_insets);
}

// Calls `fn(token)` for every non-empty `separator`-separated token of `text`.
template <typename Fn> void for_each_token(std::string_view text, char separator, Fn&& fn) {
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

} // namespace

bool SafeAreaState::set_metrics(const hal::WindowMetrics& metrics, bool reduced_motion) {
    if (has_metrics_ && same_inputs(target_, metrics)) {
        return false;
    }
    target_ = metrics;
    if (!has_metrics_ || reduced_motion) {
        current_ = metrics;
        has_metrics_ = true;
        animating_ = false;
        return true;
    }
    // Everything but the three animated inset sets follows the platform at once.
    const hal::WindowInsets bars = current_.insets;
    const hal::WindowInsets ime = current_.ime_insets;
    const hal::WindowInsets gestures = current_.gesture_insets;
    current_ = metrics;
    current_.insets = bars;
    current_.ime_insets = ime;
    current_.gesture_insets = gestures;
    animating_ = !animated_settled(current_, target_);
    return true;
}

bool SafeAreaState::step(double dt, bool reduced_motion) {
    if (!animating_) {
        return false;
    }
    if (reduced_motion || !std::isfinite(dt) || dt < 0.0) {
        const bool changed = !animated_settled(current_, target_);
        current_.insets = target_.insets;
        current_.ime_insets = target_.ime_insets;
        current_.gesture_insets = target_.gesture_insets;
        animating_ = false;
        return changed;
    }
    const double factor = 1.0 - std::exp(-dt / kTimeConstantSeconds);
    bool changed = false;
    changed |= ease(current_.insets, target_.insets, factor);
    changed |= ease(current_.ime_insets, target_.ime_insets, factor);
    changed |= ease(current_.gesture_insets, target_.gesture_insets, factor);
    animating_ = !animated_settled(current_, target_);
    return changed;
}

bool is_safe_area_node(const ui::Node& node) {
    bool found = false;
    for_each_token(node.class_name(), ' ', [&](std::string_view token) {
        found = found || token == "tsafe";
    });
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

void apply_safe_area(ui::style::ComputedStyle& style, const SafeAreaSpec& spec,
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

} // namespace tessera::app
