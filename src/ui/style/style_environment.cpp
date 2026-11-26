// SPDX-License-Identifier: MIT
//
// The environment `env()` reads (ADR-074): the container, and the standard variables computed from
// a window's metrics. The substitution itself is in var_resolve.cpp, next to `var()`.

#include "tessera/ui/style/style_environment.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>

namespace tessera::ui::style {

namespace {

// A CSS number with no exponent and at most four decimals, so `12px` stays `12px` and a fractional
// inset does not print as `12.000000px`. A non-finite input is a host bug and reads as 0.
std::string length(float value) {
    if (!std::isfinite(value)) {
        value = 0.0f;
    }
    char buffer[48];
    const auto result =
        std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, 4);
    std::string text(buffer, result.ptr);
    if (text.find('.') != std::string::npos) {
        while (text.back() == '0') {
            text.pop_back();
        }
        if (text.back() == '.') {
            text.pop_back();
        }
    }
    if (text == "-0") {
        text = "0";
    }
    return text + "px";
}

std::string number(float value) {
    std::string text = length(value);
    text.resize(text.size() - 2); // drop "px"
    return text;
}

} // namespace

std::string css_length(float px) {
    return length(px);
}

std::string css_angle(float degrees) {
    return number(degrees) + "deg";
}

bool is_continuous_environment_name(std::string_view name) {
    return name == "--tessera-hinge-angle";
}

bool differs_only_in_continuous(const StyleEnvironment& before, const StyleEnvironment& after) {
    if (before == after) {
        return false;
    }
    bool continuous_moved = false;
    const auto check = [&](const StyleEnvironment& from, const StyleEnvironment& to) {
        for (const auto& [name, value] : from.entries()) {
            const std::string* other = to.find(name);
            if (other != nullptr && *other == value) {
                continue;
            }
            if (!is_continuous_environment_name(name)) {
                return false;
            }
            continuous_moved = true;
        }
        return true;
    };
    return check(before, after) && check(after, before) && continuous_moved;
}

void StyleEnvironment::set(std::string name, std::string value) {
    const auto at = std::lower_bound(
        entries_.begin(), entries_.end(), name,
        [](const auto& entry, const std::string& key) { return entry.first < key; });
    if (at != entries_.end() && at->first == name) {
        at->second = std::move(value);
        return;
    }
    entries_.insert(at, {std::move(name), std::move(value)});
}

const std::string* StyleEnvironment::find(std::string_view name) const {
    const auto at = std::lower_bound(entries_.begin(), entries_.end(), name,
                                     [](const auto& entry, std::string_view key) {
                                         return std::string_view(entry.first) < key;
                                     });
    if (at == entries_.end() || at->first != name) {
        return nullptr;
    }
    return &at->second;
}

StyleEnvironment standard_environment(const StandardEnvironmentInputs& inputs) {
    StyleEnvironment environment;
    environment.set("safe-area-inset-top", length(inputs.safe_area_top));
    environment.set("safe-area-inset-right", length(inputs.safe_area_right));
    environment.set("safe-area-inset-bottom", length(inputs.safe_area_bottom));
    environment.set("safe-area-inset-left", length(inputs.safe_area_left));
    if (inputs.text_scale) {
        environment.set("preferred-text-scale", number(*inputs.text_scale));
    }

    // A grid that does not match its rectangles is a host bug; fall back to the one-segment
    // viewport rather than answering for a segment that has no rectangle.
    const bool grid_ok = inputs.segments_x >= 1 && inputs.segments_y >= 1 &&
                         inputs.segments.size() == inputs.segments_x * inputs.segments_y;
    EnvironmentSegment whole{0.0f, 0.0f, inputs.viewport_width, inputs.viewport_height};
    const std::size_t count_x = grid_ok ? inputs.segments_x : 1;
    const std::size_t count_y = grid_ok ? inputs.segments_y : 1;
    for (std::size_t y = 0; y < count_y; ++y) {
        for (std::size_t x = 0; x < count_x; ++x) {
            const EnvironmentSegment& segment = grid_ok ? inputs.segments[y * count_x + x] : whole;
            const std::string at = " " + std::to_string(x) + " " + std::to_string(y);
            environment.set("viewport-segment-width" + at, length(segment.width));
            environment.set("viewport-segment-height" + at, length(segment.height));
            environment.set("viewport-segment-top" + at, length(segment.y));
            environment.set("viewport-segment-left" + at, length(segment.x));
            environment.set("viewport-segment-bottom" + at, length(segment.y + segment.height));
            environment.set("viewport-segment-right" + at, length(segment.x + segment.width));
        }
    }
    return environment;
}

} // namespace tessera::ui::style
