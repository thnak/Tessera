// SPDX-License-Identifier: MIT

#include "tessera/ui/frame_interpolation.hpp"

#include <cstdio>
#include <string>

namespace tessera::ui::frame {
namespace {

// 9 significant digits round-trip a float, so a value that was parsed from a declaration reaches
// the paint pass (which re-parses it) without a visible step. Deterministic: no locale, no
// scientific notation surprises within the CSS numeric range.
std::string format_scalar(float value) {
    char buffer[32] = {};
    const int written = std::snprintf(buffer, sizeof(buffer), "%.9g", static_cast<double>(value));
    if (written <= 0) {
        return "0";
    }
    return std::string(buffer, static_cast<std::size_t>(written));
}

constexpr const char* kBorderColorProperties[4] = {"border-top-color", "border-right-color",
                                                   "border-bottom-color", "border-left-color"};

} // namespace

std::optional<rendering::rhi::Mat3> InterpolationPaintView::local_transform(NodeId node) const {
    const auto found = samples_.find(node);
    if (found == samples_.end()) {
        return std::nullopt;
    }
    return found->second.transform;
}

void InterpolationPaintView::overlay_style(NodeId node, style::ComputedStyle& style) const {
    const auto found = samples_.find(node);
    if (found == samples_.end()) {
        return;
    }
    const PaintSample& sample = found->second;
    if (sample.opacity) {
        style.set("opacity", format_scalar(*sample.opacity));
    }
    if (sample.color) {
        style.set("color", style::format_color(*sample.color));
    }
    if (sample.background_color) {
        style.set("background-color", style::format_color(*sample.background_color));
    }
    for (std::size_t index = 0; index < sample.border_colors.size(); ++index) {
        if (sample.border_colors[index]) {
            style.set(kBorderColorProperties[index],
                      style::format_color(*sample.border_colors[index]));
        }
    }
}

} // namespace tessera::ui::frame
