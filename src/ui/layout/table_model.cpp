// SPDX-License-Identifier: MIT
//
// Table structure (docs/design/table-layout.md section 2): the display classification, the
// row/group/column/caption model with anonymous boxes, the header/footer reordering and the
// `colspan`/`rowspan` placement into the cell grid.

#include "layout_length.hpp"
#include "layout_style.hpp"
#include "table_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <numeric>
#include <string>
#include <string_view>
#include <vector>

namespace tessera::ui::layout::detail {
namespace {

constexpr int kMaxSpan = 1000;

float px_value(const style::ComputedStyle& style, const char* name,
               const LayoutViewport& viewport) {
    const std::string* value = style.find(name);
    if (value == nullptr) {
        return 0.0f;
    }
    const Length length = parse_length(*value, viewport);
    return length.kind == Length::Kind::Points ? std::max(0.0f, length.value) : 0.0f;
}

} // namespace

float table_border_px(const style::ComputedStyle& style, int side, const LayoutViewport& viewport) {
    constexpr const char* widths[] = {"border-left-width", "border-top-width", "border-right-width",
                                      "border-bottom-width"};
    constexpr const char* styles[] = {"border-left-style", "border-top-style", "border-right-style",
                                      "border-bottom-style"};
    if (is_keyword(style.find(styles[side]), "none") ||
        is_keyword(style.find(styles[side]), "hidden")) {
        return 0.0f;
    }
    return px_value(style, widths[side], viewport);
}

bool is_table_container(const style::ComputedStyle& style) {
    return is_keyword(style.find("display"), "table") ||
           is_keyword(style.find("display"), "inline-table");
}

TableEngine::TableEngine(const LayoutInput& container, const LayoutViewport& viewport,
                         const GridItemLayouter& layouter)
    : container_(container),
      viewport_(viewport),
      layouter_(layouter) {
    read_style();
    build_model();
}

void TableEngine::read_style() {
    const style::ComputedStyle& style = container_.style;
    collapse_ = is_keyword(style.find("border-collapse"), "collapse");
    fixed_layout_ = is_keyword(style.find("table-layout"), "fixed");
    rtl_ = is_keyword(style.find("direction"), "rtl");
    if (!collapse_) {
        // `border-spacing`: one length for both axes, or horizontal then vertical.
        if (const std::string* value = style.find("border-spacing"); value != nullptr) {
            const std::string text = trim(*value);
            const std::size_t split = text.find_first_of(" \t");
            const Length first = parse_length(text.substr(0, split), viewport_);
            Length second = first;
            if (split != std::string::npos) {
                second = parse_length(trim(text.substr(split)), viewport_);
            }
            if (first.kind == Length::Kind::Points && second.kind == Length::Kind::Points) {
                spacing_x_ = std::max(0.0f, first.value);
                spacing_y_ = std::max(0.0f, second.value);
            }
        }
    } else {
        for (int side = 0; side < 4; ++side) {
            table_border_[side] = table_border_px(style, side, viewport_);
        }
    }
}

TableEngine::Kind TableEngine::classify(const LayoutInput& input) {
    const std::string* display = input.style.find("display");
    if (display == nullptr) {
        return Kind::Other;
    }
    struct Entry {
        std::string_view keyword;
        Kind kind;
    };
    static constexpr Entry kEntries[] = {
        {"none", Kind::Hidden},
        {"table-caption", Kind::Caption},
        {"table-column-group", Kind::ColumnGroup},
        {"table-column", Kind::Column},
        {"table-header-group", Kind::HeaderGroup},
        {"table-row-group", Kind::BodyGroup},
        {"table-footer-group", Kind::FooterGroup},
        {"table-row", Kind::Row},
        {"table-cell", Kind::Cell},
    };
    for (const Entry& entry : kEntries) {
        if (equals_keyword(trim(*display), entry.keyword)) {
            return entry.kind;
        }
    }
    return Kind::Other;
}

int TableEngine::add_row(const LayoutInput* input, int group, int bucket) {
    Row row;
    row.input = input;
    row.group = group;
    row.bucket = bucket;
    if (input != nullptr) {
        if (const std::string* value = input->style.find("height"); value != nullptr) {
            const Length length = parse_length(*value, viewport_);
            row.specified_height = length.kind == Length::Kind::Points ? length.value : 0.0f;
        }
    }
    rows_.push_back(std::move(row));
    return static_cast<int>(rows_.size()) - 1;
}

void TableEngine::add_cell(const LayoutInput& input, int row) {
    Cell cell;
    cell.input = &input;
    cell.row = row;
    cell.column_span = std::clamp<int>(input.col_span, 1, kMaxSpan);
    cell.row_span = std::min<int>(input.row_span, kMaxSpan); // 0 spans to the end of the group
    if (const std::string* value = input.style.find("width"); value != nullptr) {
        const Length length = parse_length(*value, viewport_);
        if (length.kind == Length::Kind::Points) {
            cell.fixed_width = std::max(0.0f, length.value);
        } else if (length.kind == Length::Kind::Percent) {
            cell.percent_width = std::max(0.0f, length.value);
        }
    }
    cells_.push_back(cell);
    rows_[static_cast<std::size_t>(row)].cells.push_back(static_cast<int>(cells_.size()) - 1);
}

void TableEngine::add_group(const LayoutInput& input, Kind kind) {
    Group group;
    group.input = &input;
    group.kind = kind;
    groups_.push_back(group);
    const int group_index = static_cast<int>(groups_.size()) - 1;
    int bucket = 1;
    if (kind == Kind::HeaderGroup && !seen_header_) {
        seen_header_ = true;
        bucket = 0;
    } else if (kind == Kind::FooterGroup && !seen_footer_) {
        seen_footer_ = true;
        bucket = 2;
    }
    for (const LayoutInput& child : input.children) {
        switch (classify(child)) {
        case Kind::Hidden:
            hidden_.push_back({&child, false});
            break;
        case Kind::Row: {
            const int row = add_row(&child, group_index, bucket);
            for (const LayoutInput& cell : child.children) {
                if (classify(cell) == Kind::Hidden) {
                    hidden_.push_back({&cell, false});
                } else {
                    add_cell(cell, row);
                }
            }
            break;
        }
        default: // a non-row child of a group is a one-cell anonymous row
            add_cell(child, add_row(nullptr, group_index, bucket));
            break;
        }
    }
}

void TableEngine::add_columns(const LayoutInput& input, Kind kind) {
    hidden_.push_back({&input, true});
    const auto spec_of = [this](const LayoutInput& source) {
        ColumnSpec spec;
        if (const std::string* value = source.style.find("width"); value != nullptr) {
            const Length length = parse_length(*value, viewport_);
            if (length.kind == Length::Kind::Points) {
                spec.fixed = std::max(0.0f, length.value);
            } else if (length.kind == Length::Kind::Percent) {
                spec.percent = std::max(0.0f, length.value);
            }
        }
        return spec;
    };
    const auto push = [this](const ColumnSpec& spec, int span) {
        for (int i = 0; i < std::clamp(span, 1, kMaxSpan); ++i) {
            column_specs_.push_back(spec);
        }
    };
    if (kind == Kind::Column) {
        push(spec_of(input), input.col_span);
        return;
    }
    bool has_column = false;
    for (const LayoutInput& child : input.children) {
        if (classify(child) == Kind::Column) {
            has_column = true;
            push(spec_of(child), child.col_span);
        }
    }
    if (!has_column) {
        push(spec_of(input), input.col_span);
    }
}

void TableEngine::build_model() {
    int loose_group = -1; // the anonymous group a run of loose rows shares
    for (const LayoutInput& child : container_.children) {
        const Kind kind = classify(child);
        if (kind != Kind::Row) {
            loose_group = -1;
        }
        switch (kind) {
        case Kind::Hidden:
            hidden_.push_back({&child, false});
            break;
        case Kind::Caption:
            captions_.push_back(
                {&child, is_keyword(child.style.find("caption-side"), "bottom"), 0.0f});
            break;
        case Kind::ColumnGroup:
        case Kind::Column:
            add_columns(child, kind);
            break;
        case Kind::HeaderGroup:
        case Kind::BodyGroup:
        case Kind::FooterGroup:
            add_group(child, kind);
            break;
        case Kind::Row: {
            if (loose_group < 0) {
                Group group;
                groups_.push_back(group);
                loose_group = static_cast<int>(groups_.size()) - 1;
            }
            const int row = add_row(&child, loose_group, 1);
            for (const LayoutInput& cell : child.children) {
                if (classify(cell) == Kind::Hidden) {
                    hidden_.push_back({&cell, false});
                } else {
                    add_cell(cell, row);
                }
            }
            break;
        }
        case Kind::Cell:
        case Kind::Other:
            add_cell(child, add_row(nullptr, -1, 1));
            break;
        }
    }
    finalize_order();
    place_cells();
}

// The first `thead` is shown above and the first `tfoot` below every body, wherever they sit in
// the source (CSS 2.1 17.2). Rows keep their source order inside a bucket, which keeps a group's
// rows contiguous, so only the indices need remapping.
void TableEngine::finalize_order() {
    std::vector<int> order(rows_.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [this](int lhs, int rhs) {
        return rows_[static_cast<std::size_t>(lhs)].bucket <
               rows_[static_cast<std::size_t>(rhs)].bucket;
    });
    std::vector<int> remap(rows_.size());
    std::vector<Row> sorted;
    sorted.reserve(rows_.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        remap[static_cast<std::size_t>(order[i])] = static_cast<int>(i);
        sorted.push_back(std::move(rows_[static_cast<std::size_t>(order[i])]));
    }
    rows_ = std::move(sorted);
    for (Cell& cell : cells_) {
        cell.row = remap[static_cast<std::size_t>(cell.row)];
    }
    for (Group& group : groups_) {
        group.first_row = 0;
        group.row_count = 0;
    }
    std::vector<bool> started(groups_.size(), false);
    for (std::size_t i = 0; i < rows_.size(); ++i) {
        if (rows_[i].group < 0) {
            continue;
        }
        Group& group = groups_[static_cast<std::size_t>(rows_[i].group)];
        if (!started[static_cast<std::size_t>(rows_[i].group)]) {
            started[static_cast<std::size_t>(rows_[i].group)] = true;
            group.first_row = static_cast<int>(i);
        }
        ++group.row_count;
    }
}

// CSS 2.1 17.5.2.1 cell placement: a cell takes the first free column of its row, and a rowspan
// reserves the slot in the rows below it (never past the end of its row group). A cell pushed by a
// rowspan above simply starts at the next free column.
void TableEngine::place_cells() {
    std::vector<std::vector<int>> occupied(rows_.size());
    const auto slot = [&occupied](std::size_t row, std::size_t column) -> int& {
        std::vector<int>& line = occupied[row];
        if (line.size() <= column) {
            line.resize(column + 1, -1);
        }
        return line[column];
    };
    std::size_t widest = column_specs_.size();
    for (std::size_t r = 0; r < rows_.size(); ++r) {
        std::size_t column = 0;
        for (const int index : rows_[r].cells) {
            Cell& cell = cells_[static_cast<std::size_t>(index)];
            while (slot(r, column) != -1) {
                ++column;
            }
            cell.column = static_cast<int>(column);
            // A row outside any group is alone; a grouped row can reach the end of its group.
            int last_row = static_cast<int>(r);
            if (rows_[r].group >= 0) {
                const Group& group = groups_[static_cast<std::size_t>(rows_[r].group)];
                last_row = group.first_row + group.row_count - 1;
            }
            const int available = last_row - static_cast<int>(r) + 1;
            cell.row_span = cell.row_span == 0 ? available : std::min(cell.row_span, available);
            for (int dr = 0; dr < cell.row_span; ++dr) {
                for (int dc = 0; dc < cell.column_span; ++dc) {
                    slot(r + static_cast<std::size_t>(dr), column + static_cast<std::size_t>(dc)) =
                        index;
                }
            }
            column += static_cast<std::size_t>(cell.column_span);
            widest = std::max(widest, column);
        }
    }
    column_count_ = static_cast<int>(widest);
    grid_.assign(rows_.size() * widest, -1);
    for (std::size_t r = 0; r < rows_.size(); ++r) {
        for (std::size_t c = 0; c < occupied[r].size(); ++c) {
            grid_[r * widest + c] = occupied[r][c];
        }
    }
    columns_.assign(widest, Column{});
}

} // namespace tessera::ui::layout::detail
