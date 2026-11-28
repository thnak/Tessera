// SPDX-License-Identifier: MIT

#include "tessera/ui/capsule/slot_measure.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/paint_data.hpp"

#include <algorithm>
#include <charconv>
#include <string_view>
#include <utility>

namespace tessera::ui::capsule {

namespace {

// "14px" -> 14; anything else (percent, var(), auto) is not an intrinsic size.
bool parse_px(std::string_view value, float& out) noexcept {
    if (!value.ends_with("px")) {
        return false;
    }
    value.remove_suffix(2);
    float parsed = 0.0f;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed < 0.0f) {
        return false;
    }
    out = parsed;
    return true;
}

text::TextStyle style_for(SlotTextRole role) {
    text::TextStyle style;
    style.font_size = role == SlotTextRole::Body ? 14.0f : 11.0f;
    style.line_height = role == SlotTextRole::Body ? 20.0f : 16.0f;
    return style;
}

} // namespace

SlotExtent intrinsic_slot_extent(const Node& node, const text::TextMeasurer& measurer,
                                 SlotTextRole role) {
    SlotExtent extent;
    const auto& paint = node.paint_data();
    if (paint && paint->kind == PaintKind::Text && !paint->text.empty()) {
        text::TextConstraints constraints;
        constraints.wrap = false;
        const text::TextMetrics metrics =
            measurer.measure(paint->text, style_for(role), constraints);
        extent = {metrics.width, metrics.height};
    } else {
        for (const auto& child : node.children()) {
            const SlotExtent inner = intrinsic_slot_extent(*child, measurer, role);
            extent.width += inner.width;
            extent.height = std::max(extent.height, inner.height);
        }
    }
    for (const InlineStyleDeclaration& declaration : node.inline_style()) {
        if (declaration.name == "width") {
            parse_px(declaration.value, extent.width);
        } else if (declaration.name == "height") {
            parse_px(declaration.value, extent.height);
        }
    }
    return extent;
}

SlotMeasurer make_intrinsic_slot_measurer(std::shared_ptr<const text::TextMeasurer> measurer) {
    return [measurer = std::move(measurer)](const Node& slot, SlotTextRole role) {
        return measurer ? intrinsic_slot_extent(slot, *measurer, role) : SlotExtent{};
    };
}

} // namespace tessera::ui::capsule
