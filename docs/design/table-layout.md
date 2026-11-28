# Table layout

How the `table` family of HTML tags lays out ([ADR-079](../../decisions/ADR-079-html-table-layout-engine.md),
ADR-059 group D, issue #543). The tag table is normative in
[`html-aliases.md`](../specs/jsx-intrinsics-v1/html-aliases.md); the property rows are in
[`stylesheet-property-status-v1.md`](../specs/stylesheet-property-status-v1.md). Read
[`layout.md`](layout.md) and [`grid-v1.md`](../specs/grid-v1.md) first: the table engine sits on the
same seam as the grid engine.

## 1. Shape of the solution

A table is a carrier, not a node kind. `table`, `thead`, `tbody`, `tfoot`, `tr`, `td`, `th`,
`caption`, `colgroup` and `col` all lower to `View`; what makes them a table is the computed
`display` the user-agent sheet gives each tag (`table`, `table-header-group`, `table-row-group`,
`table-footer-group`, `table-row`, `table-cell`, `table-caption`, `table-column-group`,
`table-column`). An author can write the same values on any `View`.

```text
View[display: table]          measured leaf in Yoga, like a grid
  measure callback  ->  TableEngine over the whole LayoutInput subtree
    TableEngine  ->  GridItemLayouter (flex engine) lays out each cell subtree
```

`is_grid_container` is true for a table, so the cold tree and the retained tree both treat it as a
measured leaf with an owned subtree input, and the signature of the whole subtree decides reuse. The
engine lives in `src/ui/layout/table_*.cpp`: `table_model.cpp` (structure), `table_columns.cpp` (column
widths), `table_layout.cpp` (rows, positions, boxes).

## 2. Structure

The model is built in one pass over the children of the table, classified by `display`:

| Child | Becomes |
|---|---|
| `table-caption` | a caption, above (`caption-side: top`) or below the grid |
| `table-column-group`, `table-column` | column specifications (width, `span`); no box and no space |
| `table-header-group`, `table-row-group`, `table-footer-group` | a row group holding its rows |
| `table-row` directly under the table | a row of the anonymous group shared by consecutive loose rows |
| `table-cell`, or anything else | a cell of an anonymous one-row (CSS 17.2.1 for the cases the tags can produce) |
| `display: none` | nothing; its subtree reports zero boxes at the origin |

Inside a row every non-hidden child is a cell, whatever its `display`. The first header group is
placed before every body and the first footer group after, wherever they sit in the source.

**Spans.** `td`/`th` `colSpan` and `rowSpan` and `col`/`colgroup` `span` lower to `aria-colspan` and
`aria-rowspan` (the ARIA names of the same facts), which the two builders of a layout tree copy into
`LayoutInput::col_span`/`row_span` (`apply_table_spans`; the app host and the preview use the same
function) and the layout signature hashes. A bad value is 1; a `rowspan` of 0 reaches the end of the
row group.

**Placement** is CSS 2.1 17.5.2.1: a cell takes the first free column of its row; a `rowspan` marks
its slot in the rows below, never past the end of its group; a cell pushed by a span above starts at
the next free column. The column count is the widest row or the number of `col` specifications.

## 3. Column and row sizing

Each cell is measured through the grid layouter: max-content is a layout with no constraint, and
min-content is the grid's own helper (`item_min_content_width`: a text leaf at zero width, a container
by how far its descendants reach). A cell with an authored fixed `width` is its own min and max.

**Auto layout** (the default):

1. A column's min and max are the largest of its single-column cells; an authored px width (a cell or
   a `col`) makes the column fixed, a percentage width makes it a percentage column.
2. A spanning cell that is wider than its columns widens them in proportion to their current size
   (equally when none has any), narrow spans first; columns with an authored width only grow when
   all of them have one.
3. The table width `W`: a definite width is `max(width, sum of mins)`; otherwise the table shrinks to
   fit: `min(max-content, max(min-content, available))`, where `available` is Yoga's at-most probe.
   A caption's min-content also raises the minimum.
4. Fixed and percentage columns take their width (a percentage of `W`, at least the min). The auto
   columns share the rest: all at min when there is no room, between min and max in proportion when
   there is some, and beyond max in proportion to max when there is more. With no auto column the
   surplus is spread over the others.

**Fixed layout** (`table-layout: fixed` with a definite width): widths come from `col`, then the cells
of the first row, never from content; columns without a width share what is left equally.

**Rows.** Each cell is laid out again at its final width (so text wraps at its column) with the
`fill` flag, giving its height. A row is as tall as its tallest single-row cell or its own `height`;
a spanning cell that is taller than its rows widens them in proportion. A definite table height
spreads its extra over the rows in proportion. The final cell layout is exact on both axes: a cell
always fills its slot over its own `width`/`height`.

**Spacing.** In the separate model `border-spacing` (one or two px lengths) sits between cells and
between the cells and the table edge. In the collapse model there is none.

**Vertical alignment.** A `table-cell` is a column box; `vertical-align: middle`/`bottom` maps to its
`justify-content` (centre, end), `top` and `baseline` to start. The user-agent sheet sets `middle` on
the groups and rows and cells inherit it, as in Chrome. Baseline alignment of cells is not modelled.

## 4. Collapsed borders

With `border-collapse: collapse`, cells draw their own borders, so two adjacent cells would show two
lines. The engine overlaps them: at every column and row boundary the facing borders are shared by
the narrower one's width, so the wider border is what shows, and at the table edge a cell overlaps
the table's own border the same way. The overlap per boundary is taken from the widest border of the
cells on each side, which is exact for the usual uniform border. Table width and height subtract the
overlaps; spans see the same gap.

What this does not do: resolve a conflict by border style or colour priority, or paint one border
per grid line. Where two facing borders differ in colour or style the later-painted cell wins.

## 5. Captions, boxes and direction

A caption is laid out at the table's width and stacked above or below the grid. The table is one Yoga
leaf, so a caption sits inside the table's own border and padding (a difference from CSS only for a
table with an author-set border or padding and a caption).

The engine emits one box per node id: captions and cells with their subtrees, a box for each row and
row group, and zero boxes for `col`, `colgroup` and hidden subtrees. `direction: rtl` mirrors the
columns.

A `table-*` display outside a table is a block box (no anonymous table wrapper). A table in a block
parent is shrink-to-fit through the user-agent sheet's inline marker (`align-self: -tessera-inline`).

## 6. Accessibility

The aliases inject `role="table"`, `"row"`, `"cell"` and `"columnheader"` (`thead`, `tbody`, `tfoot`,
`caption`, `colgroup`, `col` carry none). `Role` gains `Table`, `Row`, `Cell` and `ColumnHeader`:

| Role | AT-SPI | UIA | Android class |
|---|---|---|---|
| `table` | table | Table | `TableLayout` |
| `row` | filler | Group | `TableRow` |
| `cell` | table cell | DataItem | `TextView` |
| `columnheader` | table column header | HeaderItem | `TextView` |

The snapshot lists the nodes in tree order with their laid-out rectangles, and `aria-colspan`/
`aria-rowspan` stay on the node as attributes. Row headers (`scope`), `rowgroup` and the table
interfaces (row and column counts and indices) are not modelled yet.

## 7. Tests

| Test | Covers |
|---|---|
| `table_layout` | hand-computed corpus: auto sizing, spacing, colspan, rowspan, `rowspan=0`, header/footer order, captions, definite and percentage widths, `col`, fixed layout, collapse, RTL, vertical-align, anonymous rows, hidden nodes, nested tables, span signature |
| `ua_stylesheet`, `schema`, `schema_golden` | the tag defaults, alias rows, role tokens, typings |
| `a11y_table`, `a11y_atspi_mapping`, `a11y_uia_mapping` | roles in the snapshot and the platform maps |
| gallery `tables_light`/`tables_dark` | lavapipe goldens of a real page (a data table, spans, a collapsed table, a fixed table, a caption) |

## 8. Not done

Tracked in #1049: per-edge collapsed-border conflict resolution and single painting,
a caption outside the table border, the anonymous table wrapper for stray `table-*` boxes, baseline
alignment, `rowheader`/`scope` and `rowgroup` roles, and the AT-SPI/UIA table interfaces.
