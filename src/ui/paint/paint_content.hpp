// SPDX-License-Identifier: MIT

#pragma once

#include "paint_properties.hpp"

#include <algorithm>
#include <optional>

// Shared content-draw helpers for PaintData payloads: text and vector runs already share the
// placement rule, and images must not drift from vectors. Not installed.
namespace tessera::ui::paint::detail {

// The node's padded content box (border box inset by padding-*), the rect a content payload is
// painted into. Empty when the insets consume the box.
[[nodiscard]] inline std::optional<rhi::Rect>
content_box(const style::ComputedStyle& style, const rhi::Rect& border_box) {
    if (border_box.width <= 0.0f || border_box.height <= 0.0f) {
        return std::nullopt;
    }
    const float padding_left = length_value(style, "padding-left").value_or(0.0f);
    const float padding_right = length_value(style, "padding-right").value_or(0.0f);
    const float padding_top = length_value(style, "padding-top").value_or(0.0f);
    const float padding_bottom = length_value(style, "padding-bottom").value_or(0.0f);
    const float width = std::max(0.0f, border_box.width - padding_left - padding_right);
    const float height = std::max(0.0f, border_box.height - padding_top - padding_bottom);
    if (width <= 0.0f || height <= 0.0f) {
        return std::nullopt;
    }
    return rhi::Rect{border_box.x + padding_left, border_box.y + padding_top, width, height};
}

// Payload tint (white when absent) with its alpha folded with payload opacity and computed opacity.
// Nullopt when the effective alpha is zero (nothing would be painted).
[[nodiscard]] inline std::optional<rhi::Color>
content_tint(const std::optional<rhi::Color>& payload_tint, float payload_opacity,
             const style::ComputedStyle& style) {
    const float opacity = opacity_value(style) * std::clamp(payload_opacity, 0.0f, 1.0f);
    if (opacity <= 0.0f) {
        return std::nullopt;
    }
    rhi::Color tint = payload_tint.value_or(rhi::Color{1.0f, 1.0f, 1.0f, 1.0f});
    tint.a = std::clamp(tint.a, 0.0f, 1.0f) * opacity;
    if (tint.a <= 0.0f) {
        return std::nullopt;
    }
    return tint;
}

} // namespace tessera::ui::paint::detail
