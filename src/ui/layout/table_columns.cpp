// SPDX-License-Identifier: MIT
//
// Table column sizing (docs/design/table-layout.md section 3): the min/max-content contribution of
// each cell, spanning cells distributed over their columns, the table width (definite,
// shrink-to-fit or fixed layout) and the auto and fixed column algorithms.

#include "layout_style.hpp"
#include "table_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <vector>

namespace tessera::ui::layout::detail {

float TableEngine::column_chrome() const {
    const auto count = static_cast<float>(column_count_);
    if (!collapse_) {
        return (count + 1.0f) * spacing_x_;
    }
    float overlap = 0.0f;
    for (const float shared : column_overlap_) {
        overlap += shared;
    }
    return -overlap;
}

// The width a spanning cell gains over the plain sum of its columns: the spacing between them
// (separate model) or minus the borders they share (collapse model).
float TableEngine::span_gap(int first, int span) const {
    if (!collapse_) {
        return static_cast<float>(span - 1) * spacing_x_;
    }
    float gap = 0.0f;
    for (int i = 1; i < span; ++i) {
        gap -= column_overlap_[static_cast<std::size_t>(first + i)];
    }
    return gap;
}

float TableEngine::span_width(int column, int span) const {
    float width = span_gap(column, span);
    for (int i = 0; i < span; ++i) {
        width += columns_[static_cast<std::size_t>(column + i)].width;
    }
    return width;
}

// Collapse model: two adjacent cells draw their own borders, so they are overlapped by the narrower
// of the two facing borders and the wider one is what shows (design note section 4). Per column the
// widest border on each side stands for the whole boundary.
void TableEngine::compute_overlaps() {
    const auto count = static_cast<std::size_t>(column_count_);
    column_overlap_.assign(count + 1, 0.0f);
    if (!collapse_) {
        return;
    }
    // Border sides are 0 left, 1 top, 2 right, 3 bottom; the inline start side mirrors in RTL.
    const int start_side = rtl_ ? 2 : 0;
    const int end_side = rtl_ ? 0 : 2;
    std::vector<float> start_edge(count, 0.0f);
    std::vector<float> end_edge(count, 0.0f);
    for (const Cell& cell : cells_) {
        const auto first = static_cast<std::size_t>(cell.column);
        const auto last = first + static_cast<std::size_t>(cell.column_span) - 1;
        start_edge[first] = std::max(start_edge[first], cell.border[start_side]);
        end_edge[last] = std::max(end_edge[last], cell.border[end_side]);
    }
    const float table_start = table_border_[start_side];
    const float table_end = table_border_[end_side];
    column_overlap_[0] = std::min(table_start, count > 0 ? start_edge[0] : 0.0f);
    column_overlap_[count] = std::min(table_end, count > 0 ? end_edge[count - 1] : 0.0f);
    for (std::size_t c = 1; c < count; ++c) {
        column_overlap_[c] = std::min(end_edge[c - 1], start_edge[c]);
    }
}

void TableEngine::measure_cells() {
    for (const Caption& caption : captions_) {
        const GridItemLayout maximum = layouter_.layout(*caption.input, -1.0f, -1.0f, nullptr);
        caption_min_ = std::max(caption_min_,
                                item_min_content_width(layouter_, *caption.input, maximum, false));
    }
    for (Cell& cell : cells_) {
        const GridItemLayout maximum = layouter_.layout(*cell.input, -1.0f, -1.0f, nullptr);
        cell.max_width = maximum.width;
        cell.min_width =
            std::min(maximum.width, item_min_content_width(layouter_, *cell.input, maximum,
                                                           cell.fixed_width >= 0.0f));
        for (int side = 0; side < 4; ++side) {
            cell.border[side] =
                collapse_ ? table_border_px(cell.input->style, side, viewport_) : 0.0f;
        }
    }
}

// A cell spanning several columns widens them when they are too narrow for it, in proportion to
// their current size (equally when none has any). Spans are processed narrow to wide, so a wide
// span sees what the narrow ones already decided.
void TableEngine::spread_spanning(bool maximum) {
    std::vector<int> order;
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        if (cells_[i].column_span > 1) {
            order.push_back(static_cast<int>(i));
        }
    }
    std::stable_sort(order.begin(), order.end(), [this](int lhs, int rhs) {
        return cells_[static_cast<std::size_t>(lhs)].column_span <
               cells_[static_cast<std::size_t>(rhs)].column_span;
    });
    for (const int index : order) {
        const Cell& cell = cells_[static_cast<std::size_t>(index)];
        const float need =
            (maximum ? cell.max_width : cell.min_width) - span_gap(cell.column, cell.column_span);
        float have = 0.0f;
        float weight = 0.0f;
        int growable = 0;
        for (int i = 0; i < cell.column_span; ++i) {
            const Column& column = columns_[static_cast<std::size_t>(cell.column + i)];
            const float value = maximum ? column.max_width : column.min_width;
            have += value;
            if (column.fixed_width < 0.0f) {
                weight += value;
                ++growable;
            }
        }
        const float deficit = need - have;
        if (deficit <= 0.0f) {
            continue;
        }
        // Only columns without an authored width take the surplus; when every spanned column has
        // one, they share it.
        const bool all_fixed = growable == 0;
        const int takers = all_fixed ? cell.column_span : growable;
        const float total = all_fixed ? have : weight;
        for (int i = 0; i < cell.column_span; ++i) {
            Column& column = columns_[static_cast<std::size_t>(cell.column + i)];
            if (column.fixed_width >= 0.0f && !all_fixed) {
                continue;
            }
            float& stat = maximum ? column.max_width : column.min_width;
            stat += deficit * (total > 0.0f ? stat / total : 1.0f / static_cast<float>(takers));
        }
    }
}

void TableEngine::size_columns(const GridContentBox& content) {
    const auto count = static_cast<std::size_t>(column_count_);
    compute_overlaps();
    for (std::size_t c = 0; c < count; ++c) {
        if (c < column_specs_.size()) {
            columns_[c].fixed_width = column_specs_[c].fixed;
            columns_[c].percent_width = column_specs_[c].percent;
        }
        if (columns_[c].fixed_width >= 0.0f) {
            columns_[c].min_width = columns_[c].max_width = columns_[c].fixed_width;
        }
    }
    for (const Cell& cell : cells_) {
        if (cell.column_span != 1) {
            continue;
        }
        Column& column = columns_[static_cast<std::size_t>(cell.column)];
        column.min_width = std::max(column.min_width, cell.min_width);
        column.max_width = std::max(column.max_width, cell.max_width);
        if (cell.fixed_width >= 0.0f) {
            column.fixed_width = std::max(column.fixed_width, cell.fixed_width);
        }
        if (cell.percent_width >= 0.0f) {
            column.percent_width = std::max(column.percent_width, cell.percent_width);
        }
    }
    spread_spanning(false);
    spread_spanning(true);

    const float chrome = column_chrome();
    float sum_min = 0.0f;
    float sum_max = 0.0f;
    float percent_width = 0.0f; // the table width a percentage column needs to hold its content
    for (const Column& column : columns_) {
        sum_min += column.min_width;
        sum_max += column.max_width;
        if (column.percent_width > 0.0f) {
            percent_width =
                std::max(percent_width, column.max_width * 100.0f / column.percent_width + chrome);
        }
    }
    const float min_table = std::max(sum_min + chrome, caption_min_);
    const float max_table = std::max(sum_max + chrome, percent_width);
    const bool use_fixed = fixed_layout_ && content.width_definite;
    float table_width = 0.0f;
    if (content.width_definite) {
        table_width = use_fixed ? content.width : std::max(content.width, min_table);
    } else {
        table_width = max_table;
        if (table_width > content.max_width) {
            table_width = std::max(min_table, content.max_width);
        }
    }
    table_width_ = table_width;
    if (use_fixed) {
        size_columns_fixed(table_width - chrome);
    } else {
        size_columns_auto(table_width - chrome);
    }
}

void TableEngine::size_columns_auto(float target) {
    const auto count = static_cast<std::size_t>(column_count_);
    std::vector<bool> locked(count, false);
    float locked_width = 0.0f;
    float auto_min = 0.0f;
    float auto_max = 0.0f;
    std::size_t autos = 0;
    for (std::size_t c = 0; c < count; ++c) {
        Column& column = columns_[c];
        if (column.fixed_width >= 0.0f) {
            column.width = std::max(column.fixed_width, column.min_width);
            locked[c] = true;
        } else if (column.percent_width >= 0.0f) {
            column.width = std::max(column.min_width, column.percent_width * table_width_ / 100.0f);
            locked[c] = true;
        } else {
            column.width = column.min_width;
            auto_min += column.min_width;
            auto_max += column.max_width;
            ++autos;
            continue;
        }
        locked_width += column.width;
    }
    if (autos == 0) {
        // Nothing is content-sized: the surplus goes to the locked columns in proportion.
        if (locked_width > 0.0f && target > locked_width) {
            const float scale = target / locked_width;
            for (Column& column : columns_) {
                column.width *= scale;
            }
        }
        return;
    }
    const float room = target - locked_width;
    if (room <= auto_min) {
        return; // the autos stay at their min-content; the table overflows its width
    }
    for (std::size_t c = 0; c < count; ++c) {
        if (locked[c]) {
            continue;
        }
        Column& column = columns_[c];
        if (room >= auto_max) {
            const float extra = room - auto_max;
            const float share =
                auto_max > 0.0f ? column.max_width / auto_max : 1.0f / static_cast<float>(autos);
            column.width = column.max_width + extra * share;
        } else {
            const float fraction = (room - auto_min) / (auto_max - auto_min);
            column.width = column.min_width + (column.max_width - column.min_width) * fraction;
        }
    }
}

// `table-layout: fixed` (CSS 2.1 17.5.2.1): widths come from `col` and the first row only, never
// from content. Columns without a width share what is left equally.
void TableEngine::size_columns_fixed(float target) {
    const auto count = static_cast<std::size_t>(column_count_);
    std::vector<float> width(count, -1.0f);
    const auto resolve = [&](float fixed, float percent, int span) {
        if (fixed >= 0.0f) {
            return fixed / static_cast<float>(span);
        }
        if (percent >= 0.0f) {
            return percent * table_width_ / 100.0f / static_cast<float>(span);
        }
        return -1.0f;
    };
    for (std::size_t c = 0; c < count && c < column_specs_.size(); ++c) {
        width[c] = resolve(column_specs_[c].fixed, column_specs_[c].percent, 1);
    }
    if (!rows_.empty()) {
        for (const int index : rows_[0].cells) {
            const Cell& cell = cells_[static_cast<std::size_t>(index)];
            for (int i = 0; i < cell.column_span; ++i) {
                float& slot = width[static_cast<std::size_t>(cell.column + i)];
                if (slot < 0.0f) {
                    slot = resolve(cell.fixed_width, cell.percent_width, cell.column_span);
                }
            }
        }
    }
    float specified = 0.0f;
    std::size_t unspecified = 0;
    for (const float value : width) {
        if (value >= 0.0f) {
            specified += value;
        } else {
            ++unspecified;
        }
    }
    const float remaining = std::max(0.0f, target - specified);
    for (std::size_t c = 0; c < count; ++c) {
        float value = width[c] >= 0.0f ? width[c] : remaining / static_cast<float>(unspecified);
        if (unspecified == 0 && specified > 0.0f && target > specified) {
            value *= target / specified;
        }
        columns_[c].width = value;
    }
}

} // namespace tessera::ui::layout::detail
