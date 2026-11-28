// SPDX-License-Identifier: MIT
//
// Table accessibility (issue #543): the roles the `table`, `tr`, `td` and `th` aliases inject
// reach the semantics snapshot as table, row, cell and column header, in tree order with their
// text as the name and the laid-out cell rectangles as bounds.

#include "a11y_test_helpers.hpp"
#include "tessera/ui/scene/paint_data.hpp"

namespace a11y_test {
namespace {

Node& cell_of(Node& row, NodeId id, const char* role, const char* text, Rect bounds) {
    Node& cell = child(row, id);
    cell.set_element_kind("View");
    cell.set_role(role);
    cell.set_bounds(bounds);
    Node& label = child(cell, id + 100);
    label.set_element_kind("Text");
    label.set_bounds({0, 0, bounds.width, bounds.height});
    label.set_paint_data(text_payload(text));
    return cell;
}

bool table_snapshot() {
    Node root(1);
    root.set_bounds({0, 0, 300, 100});
    Node& table = child(root, 2);
    table.set_element_kind("View");
    table.set_role("table");
    table.set_bounds({0, 0, 200, 60});
    Node& header = child(table, 3);
    header.set_element_kind("View");
    header.set_role("row");
    header.set_bounds({0, 0, 200, 30});
    cell_of(header, 4, "columnheader", "Name", {0, 0, 100, 30});
    cell_of(header, 5, "columnheader", "Size", {100, 0, 100, 30});
    Node& body = child(table, 6);
    body.set_element_kind("View");
    body.set_role("row");
    body.set_bounds({0, 30, 200, 30});
    cell_of(body, 7, "cell", "tessera", {0, 0, 100, 30});
    cell_of(body, 8, "cell", "42", {100, 0, 100, 30});

    StubStyles styles;
    A11ySnapshot snapshot;
    (void)build(root, styles, snapshot);
    const auto find = [&snapshot](NodeId id) -> const A11yNode* {
        for (const A11yNode& node : snapshot.nodes) {
            if (node.id == id) {
                return &node;
            }
        }
        return nullptr;
    };
    const A11yNode* table_node = find(2);
    const A11yNode* header_row = find(3);
    const A11yNode* name_header = find(4);
    const A11yNode* data_cell = find(8);
    if (!expect(table_node != nullptr && header_row != nullptr && name_header != nullptr &&
                    data_cell != nullptr,
                "every table node is in the snapshot")) {
        return false;
    }
    return expect(table_node->role == Role::Table, "table role") &&
           expect(table_node->child_count == 2, "the table lists its two rows") &&
           expect(header_row->role == Role::Row, "row role") &&
           expect(header_row->child_count == 2, "the row lists its two cells") &&
           expect(name_header->role == Role::ColumnHeader, "columnheader role") &&
           expect(data_cell->role == Role::Cell, "cell role") &&
           expect(data_cell->bounds.x == 100.0f && data_cell->bounds.y == 30.0f &&
                      data_cell->bounds.width == 100.0f,
                  "the cell bounds are its laid-out rectangle") &&
           expect(role_from_string("columnheader") == Role::ColumnHeader,
                  "columnheader is an ARIA spelling") &&
           expect(tessera::ui::a11y::to_string(Role::Cell) == "cell", "cell has a stable name");
}

} // namespace

bool table_tests() {
    return table_snapshot();
}

} // namespace a11y_test

int main() {
    return a11y_test::table_tests() ? 0 : 1;
}
