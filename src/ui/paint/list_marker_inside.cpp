// SPDX-License-Identifier: MIT

#include "paint_list_marker.hpp"
#include "tessera/detail/text_util.hpp"
#include "tessera/ui/paint/list_marker_inside.hpp"
#include "tessera/ui/scene/node.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>

namespace tessera::ui::paint {
namespace {

constexpr const char* kListRole = "list";

bool is_text(const Node& node) {
    const auto& data = node.paint_data();
    return data && data->kind == PaintKind::Text && node.element_kind() == "Text";
}

const Node* first_text_below(const Node& node) {
    for (const auto& child : node.children()) {
        if (is_text(*child)) {
            return child.get();
        }
        if (child->role() == detail::kListItemRole || child->role() == kListRole) {
            continue;
        }
        if (const Node* found = first_text_below(*child)) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

const Node* first_item_text(const Node& item) {
    return first_text_below(item);
}

const Node* marker_host_item(const Node& text) {
    if (!is_text(text)) {
        return nullptr;
    }
    for (const Node* up = text.parent(); up != nullptr; up = up->parent()) {
        if (up->role() == detail::kListItemRole) {
            return first_item_text(*up) == &text ? up : nullptr;
        }
    }
    return nullptr;
}

std::optional<InsideMarker> inside_marker(const Node& text, StyleProvider& styles,
                                          text::TextAdvanceMeasure measure, const void* user) {
    const Node* item = marker_host_item(text);
    if (item == nullptr) {
        return std::nullopt;
    }
    InsideMarker marker;
    marker.item = item;
    marker.item_style = styles.compute(*item);
    const std::string* position = marker.item_style.find("list-style-position");
    const std::string* type_value = marker.item_style.find("list-style-type");
    if (position == nullptr || type_value == nullptr ||
        tessera::detail::lower(tessera::detail::trim(*position)) != "inside") {
        return std::nullopt;
    }
    const std::optional<style::ListStyleType> type = style::parse_list_style_type(*type_value);
    if (!type || *type == style::ListStyleType::None) {
        return std::nullopt;
    }
    marker.type = *type;
    marker.ordinal = detail::list_item_number(*item).value;
    marker.text_style = text::text_style_from(marker.item_style);
    if (style::is_bullet_marker(*type)) {
        marker.width = marker.text_style.font_size * (detail::kShapeEm + detail::kShapeGapEm);
    } else if (measure != nullptr) {
        text::TextStyle literal = marker.text_style;
        literal.white_space = text::WhiteSpace::Pre;
        literal.size_runs = {};
        marker.width = measure(style::list_marker_text(*type, marker.ordinal), literal, user);
    }
    // On the 1/16 px grid the layout input stores, rounded up so the marker never overhangs its
    // box.
    marker.width = std::min(std::ceil(marker.width * 16.0f), 65535.0f) / 16.0f;
    return marker;
}

} // namespace tessera::ui::paint
