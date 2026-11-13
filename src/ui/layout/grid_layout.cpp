// SPDX-License-Identifier: MIT
//
// Grid orchestration: style parsing, the run order, item positioning and box emission
// (docs/specs/grid-v1.md).

#include "grid_engine.hpp"
#include "layout_style.hpp"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>

namespace tessera::ui::layout::detail {
namespace {

GridAlign parse_align_value(const std::string* value, GridAlign fallback) {
    if (value == nullptr) {
        return fallback;
    }
    const std::string token = lower(trim(*value));
    if (token == "auto") {
        return fallback;
    }
    if (token == "stretch" || token == "normal" || token == "legacy") {
        return GridAlign::Stretch;
    }
    if (token == "start" || token == "flex-start") {
        return GridAlign::Start;
    }
    if (token == "end" || token == "flex-end") {
        return GridAlign::End;
    }
    if (token == "baseline") {
        // V1 has no baseline alignment for grid items; it falls back to the start edge.
        return GridAlign::Start;
    }
    if (token == "center") {
        return GridAlign::Center;
    }
    if (token == "left") {
        return GridAlign::Left;
    }
    if (token == "right") {
        return GridAlign::Right;
    }
    return fallback;
}

GridContentAlign parse_content_align(const std::string* value) {
    if (value == nullptr) {
        return GridContentAlign::Stretch;
    }
    const std::string token = lower(trim(*value));
    if (token == "start" || token == "flex-start" || token == "left") {
        return GridContentAlign::Start;
    }
    if (token == "end" || token == "flex-end" || token == "right") {
        return GridContentAlign::End;
    }
    if (token == "center") {
        return GridContentAlign::Center;
    }
    if (token == "space-between") {
        return GridContentAlign::SpaceBetween;
    }
    if (token == "space-around") {
        return GridContentAlign::SpaceAround;
    }
    if (token == "space-evenly") {
        return GridContentAlign::SpaceEvenly;
    }
    return GridContentAlign::Stretch;
}

// Offset of an item inside its grid area, measured from the area's visual start edge.
float align_offset(GridAlign align, float space, float item_size, bool rtl) {
    const float free = std::max(0.0f, space - item_size);
    switch (align) {
    case GridAlign::Start:
        return rtl ? free : 0.0f;
    case GridAlign::End:
        return rtl ? 0.0f : free;
    case GridAlign::Center:
        return free / 2.0f;
    case GridAlign::Left:
        return 0.0f;
    case GridAlign::Right:
        return free;
    case GridAlign::Stretch:
        return 0.0f;
    }
    return 0.0f;
}

bool is_content_kind(style::GridTrackKind kind) {
    return kind == style::GridTrackKind::Auto || kind == style::GridTrackKind::MinContent ||
           kind == style::GridTrackKind::MaxContent;
}

bool tracks_need_content(const std::vector<style::GridTrack>& tracks) {
    for (const style::GridTrack& track : tracks) {
        if (is_content_kind(track.min.kind) || is_content_kind(track.max.kind)) {
            return true;
        }
    }
    return false;
}

} // namespace

GridEngine::GridEngine(const LayoutInput& container, const LayoutViewport& viewport,
                       const GridItemLayouter& layouter)
    : container_(container),
      viewport_(viewport),
      layouter_(layouter) {
    parse_tracks();
    const auto margin = [this](const style::ComputedStyle& style, const char* name) {
        const std::string* value = style.find(name);
        if (value == nullptr) {
            return 0.0f;
        }
        style::GridTrackSize size;
        if (!style::parse_grid_length(*value, size)) {
            return 0.0f;
        }
        // A percentage margin resolves against the grid area's inline size, which is not known
        // while the tracks are being sized; the documented V1 behaviour is 0.
        return size.kind == style::GridTrackKind::Fixed ? resolve_length(size) : 0.0f;
    };
    items_.reserve(container_.children.size());
    for (const LayoutInput& child : container_.children) {
        if (is_keyword(child.style.find("display"), "none")) {
            continue;
        }
        Item item;
        item.input = &child;
        if (const std::string* value = child.style.find("grid-column"); value != nullptr) {
            item.column = style::parse_grid_line(*value);
        }
        if (const std::string* value = child.style.find("grid-row"); value != nullptr) {
            item.row = style::parse_grid_line(*value);
        }
        item.justify_self = parse_align_value(child.style.find("justify-self"), justify_items_);
        item.align_self = parse_align_value(child.style.find("align-self"), align_items_);
        item.width_auto = !has_definite_size(child, "width");
        item.height_auto = !has_definite_size(child, "height");
        item.margin_left = margin(child.style, "margin-left");
        item.margin_top = margin(child.style, "margin-top");
        item.margin_right = margin(child.style, "margin-right");
        item.margin_bottom = margin(child.style, "margin-bottom");
        items_.push_back(item);
    }
}

void GridEngine::parse_tracks() {
    const style::ComputedStyle& style = container_.style;
    if (const std::string* value = style.find("grid-template-columns"); value != nullptr) {
        columns_.explicit_tracks = style::parse_grid_template(*value).tracks;
    }
    if (const std::string* value = style.find("grid-template-rows"); value != nullptr) {
        rows_.explicit_tracks = style::parse_grid_template(*value).tracks;
    }
    rtl_ = is_keyword(style.find("direction"), "rtl");
    column_flow_ = is_keyword(style.find("grid-auto-flow"), "column");
    justify_items_ = parse_align_value(style.find("justify-items"), GridAlign::Stretch);
    align_items_ = parse_align_value(style.find("align-items"), GridAlign::Stretch);
    justify_content_ = parse_content_align(style.find("justify-content"));
    align_content_ = parse_content_align(style.find("align-content"));
    // An implicit track is `auto`, so a grid that can place an item outside its explicit template
    // always needs the item's content size.
    columns_.needs_content = tracks_need_content(columns_.explicit_tracks);
    rows_.needs_content = tracks_need_content(rows_.explicit_tracks);
}

void GridEngine::run(const GridContentBox& content) {
    columns_.gap = resolve_gap("column-gap", content.width);
    rows_.gap = resolve_gap("row-gap", content.height);
    place_items();
    measure_items();
    size_axis(columns_, content.width, content.width_definite, true, justify_content_);
    size_axis(rows_, content.height, content.height_definite, false, align_content_);
    content_width_ = columns_.total;
    content_height_ = rows_.total;
    position_items(content.width_definite ? content.width : columns_.total);
}

void GridEngine::position_items(float mirror_width) {
    boxes_.clear();
    for (Item& item : items_) {
        const int column = item.column_start - 1;
        const int row = item.row_start - 1;
        float cell_width = columns_.step * static_cast<float>(item.column_span - 1);
        for (int i = 0; i < item.column_span; ++i) {
            cell_width += columns_.base[static_cast<std::size_t>(column + i)];
        }
        float cell_height = rows_.step * static_cast<float>(item.row_span - 1);
        for (int i = 0; i < item.row_span; ++i) {
            cell_height += rows_.base[static_cast<std::size_t>(row + i)];
        }

        float cell_x = columns_.offsets[static_cast<std::size_t>(column)];
        const float cell_y = rows_.offsets[static_cast<std::size_t>(row)];
        if (rtl_) {
            cell_x = mirror_width - cell_x - cell_width;
        }

        const float inner_width = std::max(0.0f, cell_width - item.margin_left - item.margin_right);
        const float inner_height =
            std::max(0.0f, cell_height - item.margin_top - item.margin_bottom);

        // `stretch` fills the area; any other alignment uses the item's max-content size, clamped
        // to the area so it cannot overflow it.
        float available_width = inner_width;
        if (item.justify_self != GridAlign::Stretch && item.width_auto) {
            available_width = std::min(item.max_content_width, inner_width);
        }
        float available_height = inner_height;
        if (item.align_self != GridAlign::Stretch && item.height_auto) {
            available_height = std::min(item.max_content_height, inner_height);
        }

        item.box_begin = boxes_.size();
        const GridItemLayout placed =
            layouter_.layout(*item.input, available_width, available_height, &boxes_);
        item.box_end = boxes_.size();
        item.width = placed.width;
        item.height = placed.height;
        item.x = cell_x + item.margin_left +
                 align_offset(item.justify_self, inner_width, placed.width, rtl_);
        item.y = cell_y + item.margin_top +
                 align_offset(item.align_self, inner_height, placed.height, false);
    }
}

void GridEngine::emit_boxes(float origin_x, float origin_y, std::vector<LayoutBox>& boxes) const {
    for (const Item& item : items_) {
        for (std::size_t i = item.box_begin; i < item.box_end; ++i) {
            LayoutBox box = boxes_[i];
            box.bounds.x += origin_x + item.x;
            box.bounds.y += origin_y + item.y;
            boxes.push_back(std::move(box));
        }
    }
}

} // namespace tessera::ui::layout::detail
