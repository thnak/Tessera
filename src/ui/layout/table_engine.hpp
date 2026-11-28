// SPDX-License-Identifier: MIT
//
// The HTML/CSS table layout engine (issue #543, docs/design/table-layout.md, ADR-076). Internal to
// src/ui/layout like the grid engine: `LayoutEngine` reaches it through `run_grid`, a `display:
// table` container being a measured leaf whose subtree this engine lays out. It owns the table
// structure (groups, rows, cells, `colspan`/`rowspan`, columns), column sizing (auto and fixed
// layout), row sizing, `border-spacing`, `border-collapse`, captions and positioning; each cell
// subtree is laid out by the caller's `GridItemLayouter`, so a cell's content is still flex/block.

#pragma once

#include "grid_engine.hpp"
#include "tessera/ui/layout/layout_engine.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace tessera::ui::layout::detail {

// True for `display: table` and `display: inline-table`.
[[nodiscard]] bool is_table_container(const style::ComputedStyle& style);

// One physical border side of a box in px (0 when its style paints nothing): 0 left, 1 top, 2
// right, 3 bottom. The collapse model reads it for the table and for every cell.
[[nodiscard]] float table_border_px(const style::ComputedStyle& style, int side,
                                    const LayoutViewport& viewport);

class TableEngine {
public:
    TableEngine(const LayoutInput& container, const LayoutViewport& viewport,
                const GridItemLayouter& layouter);

    // Builds the structure, sizes columns and rows, and positions every box for `content`.
    void run(const GridContentBox& content);

    // The content-box size the table needs. Used by the container's measure callback.
    [[nodiscard]] float content_width() const {
        return content_width_;
    }
    [[nodiscard]] float content_height() const {
        return content_height_;
    }

    // Appends every positioned box (captions, groups, rows, cells and their subtrees), with the
    // content-box origin at (origin_x, origin_y).
    void emit_boxes(float origin_x, float origin_y, std::vector<LayoutBox>& boxes) const;

private:
    enum class Kind : std::uint8_t {
        Hidden,
        Caption,
        ColumnGroup,
        Column,
        HeaderGroup,
        BodyGroup,
        FooterGroup,
        Row,
        Cell,
        Other,
    };

    struct Cell {
        const LayoutInput* input = nullptr;
        int row = 0;
        int column = 0;
        int row_span = 1;
        int column_span = 1;
        float min_width = 0.0f;
        float max_width = 0.0f;
        float height = 0.0f;
        float fixed_width = -1.0f;      // an authored px width, or negative
        float percent_width = -1.0f;    // an authored percentage, or negative
        float border[4] = {0, 0, 0, 0}; // left, top, right, bottom (px, collapse model)
    };

    struct Row {
        const LayoutInput* input = nullptr; // null: an anonymous row
        int group = -1;
        int bucket = 1; // display order: 0 first header group, 1 the rest, 2 first footer group
        std::vector<int> cells;
        float specified_height = 0.0f;
        float y = 0.0f;
        float height = 0.0f;
    };

    struct Group {
        const LayoutInput* input = nullptr; // null: the anonymous group of loose rows
        Kind kind = Kind::BodyGroup;
        int first_row = 0;
        int row_count = 0;
    };

    struct ColumnSpec {
        float fixed = -1.0f;
        float percent = -1.0f;
    };

    struct Caption {
        const LayoutInput* input = nullptr;
        bool bottom = false;
        float height = 0.0f;
    };

    struct Column {
        float min_width = 0.0f;
        float max_width = 0.0f;
        float fixed_width = -1.0f;
        float percent_width = -1.0f;
        float width = 0.0f;
        float x = 0.0f;
    };

    struct Hidden {
        const LayoutInput* input = nullptr;
        bool structural = false; // a `col`/`colgroup`: no box of its own, so zero at the origin
    };

    // table_model.cpp
    [[nodiscard]] static Kind classify(const LayoutInput& input);
    void build_model();
    void add_group(const LayoutInput& input, Kind kind);
    int add_row(const LayoutInput* input, int group, int bucket);
    void add_cell(const LayoutInput& input, int row);
    void add_columns(const LayoutInput& input, Kind kind);
    void finalize_order();
    void place_cells();
    void read_style();

    // table_columns.cpp
    void measure_cells();
    void size_columns(const GridContentBox& content);
    void size_columns_auto(float target);
    void size_columns_fixed(float target);
    void spread_spanning(bool maximum);
    void compute_overlaps();
    [[nodiscard]] float column_chrome() const;
    [[nodiscard]] float span_gap(int first, int span) const;
    [[nodiscard]] float span_width(int column, int span) const;

    // table_layout.cpp
    void compute_row_overlaps();
    void size_rows(const GridContentBox& content);
    void position();
    [[nodiscard]] float row_chrome() const;
    [[nodiscard]] float row_span_gap(int first, int span) const;

    const LayoutInput& container_;
    const LayoutViewport& viewport_;
    GridItemLayouter layouter_;

    std::vector<Cell> cells_;
    std::vector<Row> rows_;
    std::vector<Group> groups_;
    std::vector<Caption> captions_;
    std::vector<Column> columns_;
    std::vector<Hidden> hidden_;
    std::vector<ColumnSpec> column_specs_; // from `col` elements, in column order
    std::vector<int> grid_;                // row * column_count -> cell index, or -1
    std::vector<float> column_overlap_;    // collapse: the shared border at each column boundary
    std::vector<float> row_overlap_;
    int column_count_ = 0;
    bool seen_header_ = false;
    bool seen_footer_ = false;

    float spacing_x_ = 0.0f;
    float spacing_y_ = 0.0f;
    bool collapse_ = false;
    bool fixed_layout_ = false;
    bool rtl_ = false;
    float table_border_[4] = {0, 0, 0, 0};

    std::vector<LayoutBox> boxes_;
    float table_width_ = 0.0f;
    float caption_min_ = 0.0f;
    float content_width_ = 0.0f;
    float content_height_ = 0.0f;
};

} // namespace tessera::ui::layout::detail
