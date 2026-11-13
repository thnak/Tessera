// SPDX-License-Identifier: MIT
//
// Item placement for the CSS Grid subset (docs/specs/grid-v1.md section 3). Sparse packing only:
// `grid-auto-flow` picks the cursor direction and `dense` is not supported.

#include "grid_engine.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace tessera::ui::layout::detail {
namespace {

// Sparse occupancy grid over the implicit grid. 1-based rows/columns keep the placement code
// aligned with CSS line numbers.
class Occupancy {
public:
    [[nodiscard]] bool is_free(int row, int column, int row_span, int column_span) const {
        if (row < 1 || column < 1 || row_span < 1 || column_span < 1) {
            return false;
        }
        if (row + row_span - 1 > rows_ || column + column_span - 1 > columns_) {
            return true;
        }
        for (int r = row; r < row + row_span; ++r) {
            for (int c = column; c < column + column_span; ++c) {
                if (cells_[static_cast<std::size_t>(r - 1) * static_cast<std::size_t>(columns_) +
                           static_cast<std::size_t>(c - 1)] != 0) {
                    return false;
                }
            }
        }
        return true;
    }

    void mark(int row, int column, int row_span, int column_span) {
        ensure(row + row_span - 1, column + column_span - 1);
        for (int r = row; r < row + row_span; ++r) {
            for (int c = column; c < column + column_span; ++c) {
                cells_[static_cast<std::size_t>(r - 1) * static_cast<std::size_t>(columns_) +
                       static_cast<std::size_t>(c - 1)] = 1;
            }
        }
    }

private:
    void ensure(int rows, int columns) {
        if (rows <= rows_ && columns <= columns_) {
            return;
        }
        const int next_rows = std::max(rows, rows_);
        const int next_columns = std::max(columns, columns_);
        std::vector<std::uint8_t> next(
            static_cast<std::size_t>(next_rows) * static_cast<std::size_t>(next_columns), 0);
        for (int row = 0; row < rows_; ++row) {
            for (int column = 0; column < columns_; ++column) {
                next[static_cast<std::size_t>(row) * static_cast<std::size_t>(next_columns) +
                     static_cast<std::size_t>(column)] =
                    cells_[static_cast<std::size_t>(row) * static_cast<std::size_t>(columns_) +
                           static_cast<std::size_t>(column)];
            }
        }
        cells_ = std::move(next);
        rows_ = next_rows;
        columns_ = next_columns;
    }

    std::vector<std::uint8_t> cells_;
    int rows_ = 0;
    int columns_ = 0;
};

// Resolves a 1-based grid line to the 1-based track the line starts. A negative line counts back
// from the last line of the explicit grid (`-1` is the line after the last explicit track); 0 is
// `auto`. A negative line that resolves before the first track is clamped to it, so a hostile value
// cannot ask for a track index that does not exist.
int resolve_line(int line, int explicit_count) {
    if (line > 0) {
        return line;
    }
    if (line < 0) {
        return std::max(1, explicit_count + 2 + line);
    }
    return 0;
}

} // namespace

void GridEngine::place_items() {
    const int explicit_columns = static_cast<int>(columns_.explicit_tracks.size());
    const int explicit_rows = static_cast<int>(rows_.explicit_tracks.size());

    for (Item& item : items_) {
        item.column_span = std::max(1, item.column.span);
        item.row_span = std::max(1, item.row.span);

        const int column_start = resolve_line(item.column.start, explicit_columns);
        const int column_end = resolve_line(item.column.end, explicit_columns);
        if (column_start > 0) {
            item.column_start = column_start;
            item.column_auto = false;
            if (column_end > column_start) {
                item.column_span = column_end - column_start;
            }
        } else if (column_end > 0) {
            item.column_start = std::max(1, column_end - item.column_span);
            item.column_auto = false;
        }

        const int row_start = resolve_line(item.row.start, explicit_rows);
        const int row_end = resolve_line(item.row.end, explicit_rows);
        if (row_start > 0) {
            item.row_start = row_start;
            item.row_auto = false;
            if (row_end > row_start) {
                item.row_span = row_end - row_start;
            }
        } else if (row_end > 0) {
            item.row_start = std::max(1, row_end - item.row_span);
            item.row_auto = false;
        }
    }

    Occupancy occupied;

    // 1. Items placed on both axes.
    for (Item& item : items_) {
        if (!item.column_auto && !item.row_auto) {
            occupied.mark(item.row_start, item.column_start, item.row_span, item.column_span);
        }
    }

    // 2./3. Items locked to one axis: the flow axis picks which one is filled first.
    const auto place_locked_to_row = [&occupied](Item& item) {
        int column = 1;
        while (!occupied.is_free(item.row_start, column, item.row_span, item.column_span)) {
            ++column;
        }
        item.column_start = column;
        item.column_auto = false;
        occupied.mark(item.row_start, item.column_start, item.row_span, item.column_span);
    };
    const auto place_locked_to_column = [&occupied](Item& item) {
        int row = 1;
        while (!occupied.is_free(row, item.column_start, item.row_span, item.column_span)) {
            ++row;
        }
        item.row_start = row;
        item.row_auto = false;
        occupied.mark(item.row_start, item.column_start, item.row_span, item.column_span);
    };
    if (column_flow_) {
        for (Item& item : items_) {
            if (!item.column_auto && item.row_auto) {
                place_locked_to_column(item);
            }
        }
        for (Item& item : items_) {
            if (item.column_auto && !item.row_auto) {
                place_locked_to_row(item);
            }
        }
    } else {
        for (Item& item : items_) {
            if (item.column_auto && !item.row_auto) {
                place_locked_to_row(item);
            }
        }
        for (Item& item : items_) {
            if (!item.column_auto && item.row_auto) {
                place_locked_to_column(item);
            }
        }
    }

    // 4. Implicit grid size known before the cursor pass.
    int column_count = std::max(explicit_columns, 1);
    int row_count = std::max(explicit_rows, 1);
    for (const Item& item : items_) {
        if (!item.column_auto) {
            column_count = std::max(column_count, item.column_start + item.column_span - 1);
        }
        if (!item.row_auto) {
            row_count = std::max(row_count, item.row_start + item.row_span - 1);
        }
    }

    // 5. Auto-placement cursor. Sparse packing: the cursor never moves backwards.
    int cursor_row = 1;
    int cursor_column = 1;
    for (Item& item : items_) {
        if (!item.column_auto || !item.row_auto) {
            continue;
        }
        if (column_flow_) {
            while (true) {
                if (cursor_row + item.row_span - 1 > row_count) {
                    ++cursor_column;
                    cursor_row = 1;
                    continue;
                }
                if (occupied.is_free(cursor_row, cursor_column, item.row_span, item.column_span)) {
                    break;
                }
                ++cursor_row;
            }
            item.row_start = cursor_row;
            item.column_start = cursor_column;
            cursor_row += item.row_span;
        } else {
            while (true) {
                if (cursor_column + item.column_span - 1 > column_count) {
                    ++cursor_row;
                    cursor_column = 1;
                    continue;
                }
                if (occupied.is_free(cursor_row, cursor_column, item.row_span, item.column_span)) {
                    break;
                }
                ++cursor_column;
            }
            item.row_start = cursor_row;
            item.column_start = cursor_column;
            cursor_column += item.column_span;
        }
        item.row_auto = false;
        item.column_auto = false;
        occupied.mark(item.row_start, item.column_start, item.row_span, item.column_span);
    }

    // 6. Final implicit grid size.
    columns_.count = std::max(explicit_columns, 1);
    rows_.count = std::max(explicit_rows, 1);
    for (const Item& item : items_) {
        columns_.count = std::max(columns_.count, item.column_start + item.column_span - 1);
        rows_.count = std::max(rows_.count, item.row_start + item.row_span - 1);
    }
}

} // namespace tessera::ui::layout::detail
