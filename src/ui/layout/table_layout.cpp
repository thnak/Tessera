// SPDX-License-Identifier: MIT
//
// Table row sizing and positioning (docs/design/table-layout.md sections 3-5): row heights from the
// cells at their final column widths, `rowspan` distribution, a definite table height spread over
// the rows, captions, and the emission of every box in content-box coordinates.

#include "layout_measure.hpp"
#include "table_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <utility>
#include <vector>

namespace tessera::ui::layout::detail {
namespace {

void append_shifted(std::vector<LayoutBox>& out, std::vector<LayoutBox>& local, float x, float y) {
    for (LayoutBox& box : local) {
        box.bounds.x += x;
        box.bounds.y += y;
        out.push_back(std::move(box));
    }
}

// Spreads `amount` over `values` in proportion to their size (equally when all are zero).
void spread(std::vector<float*>& values, float amount) {
    float total = 0.0f;
    for (const float* value : values) {
        total += *value;
    }
    for (float* value : values) {
        *value +=
            amount * (total > 0.0f ? *value / total : 1.0f / static_cast<float>(values.size()));
    }
}

} // namespace

float TableEngine::row_chrome() const {
    if (rows_.empty()) {
        return 0.0f;
    }
    if (!collapse_) {
        return (static_cast<float>(rows_.size()) + 1.0f) * spacing_y_;
    }
    float overlap = 0.0f;
    for (const float shared : row_overlap_) {
        overlap += shared;
    }
    return -overlap;
}

float TableEngine::row_span_gap(int first, int span) const {
    if (!collapse_) {
        return static_cast<float>(span - 1) * spacing_y_;
    }
    float gap = 0.0f;
    for (int i = 1; i < span; ++i) {
        gap -= row_overlap_[static_cast<std::size_t>(first + i)];
    }
    return gap;
}

// The row analogue of `compute_overlaps`: the narrower of two facing top/bottom borders is shared.
void TableEngine::compute_row_overlaps() {
    const std::size_t count = rows_.size();
    row_overlap_.assign(count + 1, 0.0f);
    if (!collapse_ || count == 0) {
        return;
    }
    std::vector<float> top_edge(count, 0.0f);
    std::vector<float> bottom_edge(count, 0.0f);
    for (const Cell& cell : cells_) {
        const auto first = static_cast<std::size_t>(cell.row);
        const auto last = first + static_cast<std::size_t>(cell.row_span) - 1;
        top_edge[first] = std::max(top_edge[first], cell.border[1]);
        bottom_edge[last] = std::max(bottom_edge[last], cell.border[3]);
    }
    row_overlap_[0] = std::min(table_border_[1], top_edge[0]);
    row_overlap_[count] = std::min(table_border_[3], bottom_edge[count - 1]);
    for (std::size_t r = 1; r < count; ++r) {
        row_overlap_[r] = std::min(bottom_edge[r - 1], top_edge[r]);
    }
}

void TableEngine::size_rows(const GridContentBox& content) {
    compute_row_overlaps();
    for (Cell& cell : cells_) {
        const float width = span_width(cell.column, cell.column_span);
        cell.height = layouter_.layout(*cell.input, width, -1.0f, nullptr, /*fill=*/true).height;
    }
    for (Row& row : rows_) {
        row.height = row.specified_height;
    }
    for (const Cell& cell : cells_) {
        if (cell.row_span == 1) {
            Row& row = rows_[static_cast<std::size_t>(cell.row)];
            row.height = std::max(row.height, cell.height);
        }
    }
    std::vector<int> spanning;
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        if (cells_[i].row_span > 1) {
            spanning.push_back(static_cast<int>(i));
        }
    }
    std::stable_sort(spanning.begin(), spanning.end(), [this](int lhs, int rhs) {
        return cells_[static_cast<std::size_t>(lhs)].row_span <
               cells_[static_cast<std::size_t>(rhs)].row_span;
    });
    for (const int index : spanning) {
        const Cell& cell = cells_[static_cast<std::size_t>(index)];
        std::vector<float*> heights;
        float have = row_span_gap(cell.row, cell.row_span);
        for (int i = 0; i < cell.row_span; ++i) {
            float& height = rows_[static_cast<std::size_t>(cell.row + i)].height;
            heights.push_back(&height);
            have += height;
        }
        if (cell.height > have) {
            spread(heights, cell.height - have);
        }
    }
    for (Caption& caption : captions_) {
        caption.height =
            layouter_.layout(*caption.input, table_width_, -1.0f, nullptr, /*fill=*/true).height;
    }
    if (content.height_definite && !rows_.empty()) {
        float captions = 0.0f;
        for (const Caption& caption : captions_) {
            captions += caption.height;
        }
        float total = row_chrome() + captions;
        std::vector<float*> heights;
        for (Row& row : rows_) {
            heights.push_back(&row.height);
            total += row.height;
        }
        if (content.height > total) {
            spread(heights, content.height - total);
        }
    }
}

void TableEngine::position() {
    const auto count = static_cast<std::size_t>(column_count_);
    float cursor = collapse_ ? -column_overlap_[0] : spacing_x_;
    for (std::size_t c = 0; c < count; ++c) {
        columns_[c].x = cursor;
        cursor += columns_[c].width + (collapse_ ? -column_overlap_[c + 1] : spacing_x_);
    }
    content_width_ = count == 0 ? table_width_ : std::max(table_width_, cursor);
    const float width = content_width_;
    const auto mirror = [this, width](float x, float w) { return rtl_ ? width - x - w : x; };

    boxes_.clear();
    float y = 0.0f;
    for (const Caption& caption : captions_) {
        if (caption.bottom) {
            continue;
        }
        std::vector<LayoutBox> local;
        (void)layouter_.layout(*caption.input, width, caption.height, &local, /*fill=*/true);
        append_shifted(boxes_, local, 0.0f, y);
        y += caption.height;
    }
    float row_cursor = y + (rows_.empty() ? 0.0f : (collapse_ ? -row_overlap_[0] : spacing_y_));
    for (std::size_t r = 0; r < rows_.size(); ++r) {
        rows_[r].y = row_cursor;
        row_cursor += rows_[r].height + (collapse_ ? -row_overlap_[r + 1] : spacing_y_);
    }
    y = rows_.empty() ? y : std::max(y, row_cursor);
    for (const Caption& caption : captions_) {
        if (!caption.bottom) {
            continue;
        }
        std::vector<LayoutBox> local;
        (void)layouter_.layout(*caption.input, width, caption.height, &local, /*fill=*/true);
        append_shifted(boxes_, local, 0.0f, y);
        y += caption.height;
    }
    content_height_ = y;

    for (const Group& group : groups_) {
        if (group.input == nullptr || group.row_count == 0) {
            continue;
        }
        const Row& first = rows_[static_cast<std::size_t>(group.first_row)];
        const Row& last = rows_[static_cast<std::size_t>(group.first_row + group.row_count - 1)];
        boxes_.push_back(LayoutBox{group.input->id,
                                   rhi::Rect{0.0f, first.y, width, last.y + last.height - first.y},
                                   LayoutInsets{}});
    }
    for (const Row& row : rows_) {
        if (row.input != nullptr) {
            boxes_.push_back(LayoutBox{row.input->id, rhi::Rect{0.0f, row.y, width, row.height},
                                       LayoutInsets{}});
        }
    }
    for (const Cell& cell : cells_) {
        const float cell_width = span_width(cell.column, cell.column_span);
        const Row& first = rows_[static_cast<std::size_t>(cell.row)];
        float cell_height = row_span_gap(cell.row, cell.row_span);
        for (int i = 0; i < cell.row_span; ++i) {
            cell_height += rows_[static_cast<std::size_t>(cell.row + i)].height;
        }
        std::vector<LayoutBox> local;
        (void)layouter_.layout(*cell.input, cell_width, cell_height, &local, /*fill=*/true);
        append_shifted(boxes_, local,
                       mirror(columns_[static_cast<std::size_t>(cell.column)].x, cell_width),
                       first.y);
    }
    // `display: none`, `col` and `colgroup` take no space: zero boxes at the origin keep every
    // node id answered, as the flex and grid paths do.
    for (const Hidden& hidden : hidden_) {
        LayoutResult zero;
        emit_zero_boxes(*hidden.input, 0.0f, 0.0f, zero);
        for (LayoutBox& box : zero.boxes) {
            boxes_.push_back(std::move(box));
        }
    }
}

void TableEngine::run(const GridContentBox& content) {
    measure_cells();
    size_columns(content);
    size_rows(content);
    position();
}

void TableEngine::emit_boxes(float origin_x, float origin_y, std::vector<LayoutBox>& boxes) const {
    for (const LayoutBox& source : boxes_) {
        LayoutBox box = source;
        box.bounds.x += origin_x;
        box.bounds.y += origin_y;
        boxes.push_back(std::move(box));
    }
}

} // namespace tessera::ui::layout::detail
