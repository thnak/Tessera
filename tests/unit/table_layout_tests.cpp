// SPDX-License-Identifier: MIT
//
// Deterministic table layout corpus (issue #543): auto and fixed layout, `colspan`/`rowspan`,
// border-spacing and border-collapse, captions, header/footer groups, columns, RTL, vertical
// alignment and nesting. Every expected box is worked out by hand from docs/design/table-layout.md;
// cells have explicit sizes, so nothing here depends on a text measurer.

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/layout/layout_signature.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutEngine;
using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::style::ComputedStyle;

using Props = std::initializer_list<std::pair<const char*, const char*>>;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "table: " << message << "\n";
    }
    return condition;
}

ComputedStyle style(Props values) {
    ComputedStyle result;
    for (const auto& [property, value] : values) {
        result.set(property, value);
    }
    return result;
}

LayoutInput node(std::uint64_t id, Props values, std::vector<LayoutInput> children = {}) {
    LayoutInput result;
    result.id = id;
    result.style = style(values);
    result.children = std::move(children);
    return result;
}

LayoutInput with_spans(LayoutInput input, std::uint16_t columns, std::uint16_t rows) {
    input.col_span = columns;
    input.row_span = rows;
    return input;
}

// A cell with an explicit border box; `display: table-cell` is what a `td` gets from the sheet.
LayoutInput cell(std::uint64_t id, const char* width, const char* height) {
    return node(id, {{"display", "table-cell"}, {"width", width}, {"height", height}});
}

// A cell with no content size: its column takes what the table gives it.
LayoutInput empty_cell(std::uint64_t id, const char* height = "10px") {
    return node(id, {{"display", "table-cell"}, {"height", height}});
}

LayoutInput row(std::uint64_t id, std::vector<LayoutInput> cells) {
    return node(id, {{"display", "table-row"}}, std::move(cells));
}

LayoutInput group(std::uint64_t id, const char* display, std::vector<LayoutInput> rows) {
    return node(id, {{"display", display}}, std::move(rows));
}

// The wrapper is a flex column with `flex-start`, so the table is shrink-to-fit as in a block
// parent with the user-agent sheet's inline marker.
LayoutInput wrap(LayoutInput table) {
    std::vector<LayoutInput> children;
    children.push_back(std::move(table));
    return node(1,
                {{"display", "flex"},
                 {"flex-direction", "column"},
                 {"align-items", "flex-start"},
                 {"width", "400px"}},
                std::move(children));
}

const LayoutBox* find(const LayoutResult& result, std::uint64_t id) {
    for (const LayoutBox& box : result.boxes) {
        if (box.id == id) {
            return &box;
        }
    }
    return nullptr;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.01f;
}

bool box_is(const LayoutResult& result, std::uint64_t id, float x, float y, float width,
            float height) {
    const LayoutBox* box = find(result, id);
    const bool matched = box != nullptr && near(box->bounds.x, x) && near(box->bounds.y, y) &&
                         near(box->bounds.width, width) && near(box->bounds.height, height);
    if (!matched) {
        std::cerr << "  box " << id << " expected " << x << "," << y << " " << width << "x"
                  << height;
        if (box == nullptr) {
            std::cerr << " got none\n";
        } else {
            std::cerr << " got " << box->bounds.x << "," << box->bounds.y << " "
                      << box->bounds.width << "x" << box->bounds.height << "\n";
        }
    }
    return matched;
}

LayoutResult run(const LayoutInput& root) {
    const LayoutEngine engine;
    return engine.layout(root, LayoutViewport{400.0f, 300.0f});
}

bool auto_layout_and_spacing() {
    const auto build = [](const char* spacing) {
        ComputedStyle table_style = style({{"display", "table"}});
        if (spacing != nullptr) {
            table_style.set("border-spacing", spacing);
        }
        LayoutInput table;
        table.id = 100;
        table.style = std::move(table_style);
        table.children.push_back(row(110, {cell(111, "50px", "20px"), cell(112, "80px", "30px")}));
        table.children.push_back(row(120, {cell(121, "60px", "10px"), cell(122, "40px", "10px")}));
        return wrap(std::move(table));
    };
    // Columns take the widest cell (60, 80), rows the tallest (30, 10); every cell fills its slot.
    const LayoutResult plain = run(build(nullptr));
    if (!expect(box_is(plain, 100, 0, 0, 140, 40), "table shrinks to its columns") ||
        !expect(box_is(plain, 110, 0, 0, 140, 30), "row 1 box") ||
        !expect(box_is(plain, 120, 0, 30, 140, 10), "row 2 box") ||
        !expect(box_is(plain, 111, 0, 0, 60, 30), "cell (1,1) fills its column and row") ||
        !expect(box_is(plain, 112, 60, 0, 80, 30), "cell (1,2)") ||
        !expect(box_is(plain, 121, 0, 30, 60, 10), "cell (2,1)") ||
        !expect(box_is(plain, 122, 60, 30, 80, 10), "cell (2,2)")) {
        return false;
    }
    // `border-spacing: 4px 6px`: three horizontal and three vertical gaps around the cells.
    const LayoutResult spaced = run(build("4px 6px"));
    return expect(box_is(spaced, 100, 0, 0, 152, 58), "spaced table size") &&
           expect(box_is(spaced, 111, 4, 6, 60, 30), "spaced cell (1,1)") &&
           expect(box_is(spaced, 112, 68, 6, 80, 30), "spaced cell (1,2)") &&
           expect(box_is(spaced, 121, 4, 42, 60, 10), "spaced cell (2,1)") &&
           expect(box_is(spaced, 122, 68, 42, 80, 10), "spaced cell (2,2)") &&
           expect(box_is(spaced, 110, 0, 6, 152, 30), "spaced row box");
}

bool spans() {
    // A two-column cell wider than the columns widens both equally (they have no content).
    LayoutInput table = node(100, {{"display", "table"}});
    table.children.push_back(row(110, {with_spans(cell(111, "200px", "10px"), 2, 1)}));
    table.children.push_back(row(120, {empty_cell(121), empty_cell(122)}));
    const LayoutResult colspan = run(wrap(std::move(table)));
    if (!expect(box_is(colspan, 100, 0, 0, 200, 20), "colspan table") ||
        !expect(box_is(colspan, 111, 0, 0, 200, 10), "colspan cell spans both") ||
        !expect(box_is(colspan, 121, 0, 10, 100, 10), "colspan column 1") ||
        !expect(box_is(colspan, 122, 100, 10, 100, 10), "colspan column 2")) {
        return false;
    }
    // A rowspan reserves its slot below, so the next row's cell starts in the second column, and
    // the spanning cell's extra height is shared by the rows it covers.
    LayoutInput tall = node(200, {{"display", "table"}});
    tall.children.push_back(
        row(210, {with_spans(cell(211, "30px", "50px"), 1, 2), cell(212, "40px", "10px")}));
    tall.children.push_back(row(220, {cell(221, "40px", "10px")}));
    const LayoutResult rowspan = run(wrap(std::move(tall)));
    if (!expect(box_is(rowspan, 200, 0, 0, 70, 50), "rowspan table") ||
        !expect(box_is(rowspan, 211, 0, 0, 30, 50), "rowspan cell covers both rows") ||
        !expect(box_is(rowspan, 212, 30, 0, 40, 25), "rowspan row 1 cell") ||
        !expect(box_is(rowspan, 221, 30, 25, 40, 25), "next row skips the spanned column")) {
        return false;
    }
    // `rowspan="0"` runs to the end of the row group.
    LayoutInput open = node(300, {{"display", "table"}});
    open.children.push_back(
        group(310, "table-row-group",
              {row(311, {with_spans(cell(312, "30px", "10px"), 1, 0), cell(313, "40px", "10px")}),
               row(314, {cell(315, "40px", "10px")}), row(316, {cell(317, "40px", "10px")})}));
    const LayoutResult to_end = run(wrap(std::move(open)));
    return expect(box_is(to_end, 312, 0, 0, 30, 30), "rowspan 0 spans the group") &&
           expect(box_is(to_end, 317, 30, 20, 40, 10), "rowspan 0 third row");
}

bool groups_and_captions() {
    // The footer is written before the header and the body, and still ends up last; the header
    // leads.
    LayoutInput table = node(100, {{"display", "table"}});
    table.children.push_back(
        group(110, "table-footer-group", {row(111, {cell(112, "20px", "10px")})}));
    table.children.push_back(
        group(120, "table-row-group", {row(121, {cell(122, "20px", "10px")})}));
    table.children.push_back(
        group(130, "table-header-group", {row(131, {cell(132, "20px", "10px")})}));
    const LayoutResult grouped = run(wrap(std::move(table)));
    if (!expect(box_is(grouped, 132, 0, 0, 20, 10), "header first") ||
        !expect(box_is(grouped, 122, 0, 10, 20, 10), "body second") ||
        !expect(box_is(grouped, 112, 0, 20, 20, 10), "footer last") ||
        !expect(box_is(grouped, 130, 0, 0, 20, 10), "header group box") ||
        !expect(box_is(grouped, 110, 0, 20, 20, 10), "footer group box")) {
        return false;
    }
    // A caption is as wide as the table, above or below the grid.
    const auto captioned = [](const char* side) {
        LayoutInput table = node(200, {{"display", "table"}});
        table.children.push_back(
            node(210, {{"display", "table-caption"}, {"height", "12px"}, {"caption-side", side}}));
        table.children.push_back(row(220, {cell(221, "100px", "30px")}));
        return wrap(std::move(table));
    };
    const LayoutResult top = run(captioned("top"));
    const LayoutResult bottom = run(captioned("bottom"));
    return expect(box_is(top, 200, 0, 0, 100, 42), "caption table height") &&
           expect(box_is(top, 210, 0, 0, 100, 12), "caption on top") &&
           expect(box_is(top, 221, 0, 12, 100, 30), "grid under the caption") &&
           expect(box_is(bottom, 210, 0, 30, 100, 12), "caption on the bottom") &&
           expect(box_is(bottom, 221, 0, 0, 100, 30), "grid above the caption");
}

bool widths() {
    // A definite table width is shared by content-less columns equally.
    LayoutInput table = node(100, {{"display", "table"}, {"width", "300px"}});
    table.children.push_back(row(110, {empty_cell(111), empty_cell(112)}));
    const LayoutResult definite = run(wrap(std::move(table)));
    if (!expect(box_is(definite, 100, 0, 0, 300, 10), "definite width") ||
        !expect(box_is(definite, 111, 0, 0, 150, 10), "definite column 1") ||
        !expect(box_is(definite, 112, 150, 0, 150, 10), "definite column 2")) {
        return false;
    }
    // A percentage column takes its share of the table; the auto column takes the rest.
    LayoutInput percent = node(200, {{"display", "table"}, {"width", "400px"}});
    percent.children.push_back(
        row(210, {node(211, {{"display", "table-cell"}, {"width", "25%"}, {"height", "10px"}}),
                  empty_cell(212)}));
    const LayoutResult shared = run(wrap(std::move(percent)));
    if (!expect(box_is(shared, 211, 0, 0, 100, 10), "percentage column") ||
        !expect(box_is(shared, 212, 100, 0, 300, 10), "auto column takes the rest")) {
        return false;
    }
    // `col` sets a column's width.
    LayoutInput columns = node(300, {{"display", "table"}});
    columns.children.push_back(node(310, {{"display", "table-column-group"}},
                                    {node(311, {{"display", "table-column"}, {"width", "70px"}})}));
    columns.children.push_back(row(320, {empty_cell(321), empty_cell(322)}));
    const LayoutResult from_col = run(wrap(std::move(columns)));
    return expect(box_is(from_col, 300, 0, 0, 70, 10), "col width sizes the table") &&
           expect(box_is(from_col, 321, 0, 0, 70, 10), "col width sizes the column") &&
           expect(box_is(from_col, 322, 70, 0, 0, 10), "the second column has no width") &&
           expect(find(from_col, 311) != nullptr && find(from_col, 310) != nullptr,
                  "col and colgroup answer their ids");
}

bool fixed_layout() {
    // Widths come from the first row; the wide cell below does not matter.
    LayoutInput table =
        node(100, {{"display", "table"}, {"table-layout", "fixed"}, {"width", "300px"}});
    table.children.push_back(
        row(110, {cell(111, "100px", "10px"), empty_cell(112), empty_cell(113)}));
    table.children.push_back(
        row(120, {cell(121, "250px", "10px"), empty_cell(122), empty_cell(123)}));
    const LayoutResult fixed = run(wrap(std::move(table)));
    return expect(box_is(fixed, 100, 0, 0, 300, 20), "fixed table width") &&
           expect(box_is(fixed, 111, 0, 0, 100, 10), "fixed column 1") &&
           expect(box_is(fixed, 112, 100, 0, 100, 10), "fixed column 2 takes half the rest") &&
           expect(box_is(fixed, 113, 200, 0, 100, 10), "fixed column 3") &&
           expect(box_is(fixed, 121, 0, 10, 100, 10), "fixed ignores later rows");
}

bool collapse() {
    // Two 2px borders that face each other are one 2px line: the cells overlap by 2px.
    const auto bordered = [](std::uint64_t id) {
        return node(id, {{"display", "table-cell"},
                         {"box-sizing", "border-box"},
                         {"width", "40px"},
                         {"height", "20px"},
                         {"border-left-width", "2px"},
                         {"border-left-style", "solid"},
                         {"border-right-width", "2px"},
                         {"border-right-style", "solid"},
                         {"border-top-width", "2px"},
                         {"border-top-style", "solid"},
                         {"border-bottom-width", "2px"},
                         {"border-bottom-style", "solid"}});
    };
    LayoutInput table = node(100, {{"display", "table"}, {"border-collapse", "collapse"}});
    table.children.push_back(row(110, {bordered(111), bordered(112)}));
    table.children.push_back(row(120, {bordered(121), bordered(122)}));
    const LayoutResult collapsed = run(wrap(std::move(table)));
    return expect(box_is(collapsed, 100, 0, 0, 78, 38), "collapsed table size") &&
           expect(box_is(collapsed, 111, 0, 0, 40, 20), "collapsed cell (1,1)") &&
           expect(box_is(collapsed, 112, 38, 0, 40, 20), "collapsed cell (1,2) overlaps") &&
           expect(box_is(collapsed, 121, 0, 18, 40, 20), "collapsed cell (2,1) overlaps") &&
           expect(box_is(collapsed, 122, 38, 18, 40, 20), "collapsed cell (2,2)");
}

bool direction_and_alignment() {
    LayoutInput rtl = node(100, {{"display", "table"}, {"direction", "rtl"}});
    rtl.children.push_back(row(110, {cell(111, "60px", "10px"), cell(112, "80px", "10px")}));
    const LayoutResult mirrored = run(wrap(std::move(rtl)));
    if (!expect(box_is(mirrored, 111, 80, 0, 60, 10), "rtl first column on the right") ||
        !expect(box_is(mirrored, 112, 0, 0, 80, 10), "rtl second column on the left")) {
        return false;
    }
    // `vertical-align: middle` centres the content in the row's height.
    const auto aligned = [](const char* alignment) {
        LayoutInput content = node(121, {{"width", "20px"}, {"height", "10px"}});
        LayoutInput holder = node(
            120, {{"display", "table-cell"}, {"width", "30px"}, {"vertical-align", alignment}}, {});
        holder.children.push_back(std::move(content));
        LayoutInput table = node(100, {{"display", "table"}});
        table.children.push_back(row(110, {std::move(holder), cell(122, "30px", "30px")}));
        return wrap(std::move(table));
    };
    const LayoutResult middle = run(aligned("middle"));
    const LayoutResult bottom = run(aligned("bottom"));
    const LayoutResult top = run(aligned("top"));
    return expect(box_is(middle, 121, 0, 10, 20, 10), "middle") &&
           expect(box_is(bottom, 121, 0, 20, 20, 10), "bottom") &&
           expect(box_is(top, 121, 0, 0, 20, 10), "top");
}

bool structure() {
    // A cell written straight under the table becomes a one-cell anonymous row; `display: none`
    // takes no space but still answers its id.
    LayoutInput table = node(100, {{"display", "table"}});
    table.children.push_back(cell(110, "50px", "10px"));
    table.children.push_back(node(120, {{"display", "none"}}, {node(121, {})}));
    table.children.push_back(row(130, {cell(131, "50px", "10px")}));
    const LayoutResult loose = run(wrap(std::move(table)));
    if (!expect(box_is(loose, 100, 0, 0, 50, 20), "anonymous row table") ||
        !expect(box_is(loose, 110, 0, 0, 50, 10), "loose cell") ||
        !expect(box_is(loose, 131, 0, 10, 50, 10), "row after the loose cell") ||
        !expect(box_is(loose, 120, 0, 0, 0, 0), "hidden child is zero") ||
        !expect(box_is(loose, 121, 0, 0, 0, 0), "hidden grandchild is zero")) {
        return false;
    }
    // An empty table is empty, and a table inside a cell sizes the outer column.
    const LayoutResult empty = run(wrap(node(200, {{"display", "table"}})));
    if (!expect(box_is(empty, 200, 0, 0, 0, 0), "empty table")) {
        return false;
    }
    LayoutInput inner = node(310, {{"display", "table"}});
    inner.children.push_back(row(311, {cell(312, "40px", "10px")}));
    LayoutInput holder = node(301, {{"display", "table-cell"}}, {});
    holder.children.push_back(std::move(inner));
    LayoutInput outer = node(300, {{"display", "table"}});
    outer.children.push_back(row(302, {std::move(holder)}));
    const LayoutResult nested = run(wrap(std::move(outer)));
    return expect(box_is(nested, 300, 0, 0, 40, 10), "nested table sizes the outer one") &&
           expect(box_is(nested, 312, 0, 0, 40, 10), "nested cell");
}

bool signature_covers_spans() {
    const LayoutViewport viewport{400.0f, 300.0f};
    LayoutInput one = node(1, {{"display", "table-cell"}});
    LayoutInput two = one;
    two.col_span = 2;
    LayoutInput three = one;
    three.row_span = 0;
    const std::uint64_t base = tessera::ui::layout::layout_signature(one, viewport);
    return expect(base != tessera::ui::layout::layout_signature(two, viewport),
                  "colspan changes the layout signature") &&
           expect(base != tessera::ui::layout::layout_signature(three, viewport),
                  "rowspan changes the layout signature");
}

} // namespace

int main() {
    const bool ok = auto_layout_and_spacing() && spans() && groups_and_captions() && widths() &&
                    fixed_layout() && collapse() && direction_and_alignment() && structure() &&
                    signature_covers_spans();
    return ok ? 0 : 1;
}
