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
    animating_ = !settled(bars, metrics.insets) || !settled(ime, metrics.ime_insets) ||
                 !settled(gestures, metrics.gesture_insets);
    return true;
}

bool SafeAreaState::step(double dt, bool reduced_motion) {
    if (!animating_) {
        return false;
    }
    if (reduced_motion || !std::isfinite(dt) || dt < 0.0) {
        const bool changed = !settled(current_.insets, target_.insets) ||
                             !settled(current_.ime_insets, target_.ime_insets) ||
                             !settled(current_.gesture_insets, target_.gesture_insets);
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
    animating_ = !settled(current_.insets, target_.insets) ||
                 !settled(current_.ime_insets, target_.ime_insets) ||
                 !settled(current_.gesture_insets, target_.gesture_insets);
    return changed;
}

namespace {

std::string_view inline_value(const ui::Node& node, std::string_view name) {
    for (const ui::Node::InlineStyleDeclaration& declaration : node.inline_style()) {
        if (declaration.name == name) {
            return declaration.value;
        }
    }
    return {};
}

template <typename Fn> void for_each_word(std::string_view text, Fn&& fn) {
    std::size_t index = 0;
    while (index < text.size()) {
        while (index < text.size() && text[index] == ' ') {
            ++index;
        }
        const std::size_t start = index;
        while (index < text.size() && text[index] != ' ') {
            ++index;
        }
        if (index > start) {
            fn(text.substr(start, index - start));
        }
    }
}

float parse_px(std::string_view text) noexcept {
    if (text.ends_with("px")) {
        text.remove_suffix(2);
    }
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

} // namespace

bool is_safe_area_node(const ui::Node& node) {
    return inline_value(node, "--safe-area") == "on";
}

std::optional<SafeAreaSpec> read_safe_area_spec(const ui::Node& node, bool rtl) {
    if (inline_value(node, "--safe-area") != "on") {
        return std::nullopt;
    }
    SafeAreaSpec spec;
    spec.config.rtl = rtl;
    if (const std::string_view edges = inline_value(node, "--safe-edges"); !edges.empty()) {
        spec.config.edges = 0;
        for_each_word(edges, [&](std::string_view word) {
            if (word == "top") {
                spec.config.edges |= hal::kEdgeTop;
            } else if (word == "right") {
                spec.config.edges |= hal::kEdgeRight;
            } else if (word == "bottom") {
                spec.config.edges |= hal::kEdgeBottom;
            } else if (word == "left") {
                spec.config.edges |= hal::kEdgeLeft;
            } else if (word == "start") {
                spec.config.edges |= hal::kEdgeStart;
            } else if (word == "end") {
                spec.config.edges |= hal::kEdgeEnd;
            }
        });
    }
    if (const std::string_view sources = inline_value(node, "--safe-sources"); !sources.empty()) {
        spec.config.sources = 0;
        for_each_word(sources, [&](std::string_view word) {
            if (word == "system-bars") {
                spec.config.sources |= hal::kSourceSystemBars;
            } else if (word == "cutout") {
                spec.config.sources |= hal::kSourceCutout;
            } else if (word == "ime") {
                spec.config.sources |= hal::kSourceIme;
            } else if (word == "gestures") {
                spec.config.sources |= hal::kSourceGestures;
            } else if (word == "hinge") {
                spec.config.sources |= hal::kSourceHinge;
            }
        });
    }
    spec.config.minimum = parse_px(inline_value(node, "--safe-minimum"));
    spec.margin = inline_value(node, "--safe-mode") == "margin";
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
