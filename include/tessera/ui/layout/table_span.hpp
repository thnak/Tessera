// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/ui/layout/layout_engine.hpp"

namespace tessera::ui {
class Node;
}

namespace tessera::ui::layout {

// Fills `input.col_span`/`input.row_span` from the node's `aria-colspan`/`aria-rowspan` (what the
// `td`/`th` `colSpan`/`rowSpan` and the `col`/`colgroup` `span` attributes lower to,
// docs/design/table-layout.md section 2). A missing or malformed value is the default of 1; a
// `rowspan` of 0 stays 0 (to the end of the row group). Both builders of a layout tree call it, so
// the app and the preview read spans the same way.
void apply_table_spans(LayoutInput& input, const Node& node);

} // namespace tessera::ui::layout
