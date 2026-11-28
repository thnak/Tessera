# ADR-079: HTML tables are a carrier-based layout engine over the grid seam

- Status: Accepted (2026-11-28), implementing [ADR-059](ADR-059-html-tags-supported-not-rejected.md)
  group D (owner answer 3: tables are implemented completely, with no approximate first slice).
- Date: 2026-11-28
- Related: [ADR-059](ADR-059-html-tags-supported-not-rejected.md) (section 2 requires a recorded
  decision for carrier versus node kind), [ADR-005](ADR-005-yoga-layout-adapter.md) (Yoga adapter),
  [ADR-039](ADR-039-lowercase-html-tags-accepted.md) (lowercase tags are aliases),
  [`grid-v1.md`](../docs/specs/grid-v1.md) (the engine seam this reuses),
  [`table-layout.md`](../docs/design/table-layout.md) (the algorithms), issue #543.
- Terms: a **carrier** is an existing intrinsic (`View`) that a tag lowers to, with an injected
  `role` and a user-agent style, as `ul` and `li` do. A **node kind** would be a new intrinsic with its
  own scene element.

## Context

ADR-059 left one question open for tables: is a table a carrier, or does it need a node kind of its
own ("if a tag cannot be carried this way without distorting a carrier (tables are the likely
case)")? The layout is the hard part either way: content-driven column sizing, `colspan`/`rowspan`,
captions, header and footer groups, `border-collapse`. Yoga has no table mode, and CSS Grid cannot
size a column from the cells of other rows that span it.

The facts this builds on:

- The grid engine already shows how to lay out a container Yoga cannot: the container is a Yoga
  leaf, its measure callback runs an engine over the whole `LayoutInput` subtree, and each item
  subtree is laid out by the flex engine through a callback (`GridItemLayouter`). Both the cold and
  the retained tree already dispatch on `is_grid_container`.
- `display` is a computed style, so `display: table`, `table-row`, `table-cell` ... can be set by the
  user-agent sheet from the tag the alias keeps, exactly as `pre`, `blockquote` and `hr` are.
- `aria-colspan` and `aria-rowspan` are the ARIA names of the spans, and `aria-start` already
  shows that an alias can rename a tag attribute to an `aria-*` pair.

## Decision

1. **Carrier, not a node kind.** `table`, `thead`, `tbody`, `tfoot`, `tr`, `td`, `th`, `caption`,
   `colgroup` and `col` lower to `View`. The roles `table`, `row`, `cell` and `columnheader` are
   injected (the other tags have no role), the user-agent sheet gives each tag its `display` value, and
   an author can write the same `display: table*` values on any `View`. There is no new scene element,
   no new IR node and no new paint path: a table is boxes with ids like every other tree.
2. **The layout is an engine behind the existing seam.** A `display: table` (or `inline-table`)
   container is a measured leaf like a grid; `TableEngine` builds the table model from the subtree and
   lays each cell out through the same `GridItemLayouter` (with a `fill` flag so a cell fills its
   column and row). The engine is internal to `src/ui/layout`; `LayoutEngine`'s public surface does not
   change.
3. **Spans reach layout through `LayoutInput`.** `colSpan`, `rowSpan` and `span` rewrite to
   `aria-colspan`/`aria-rowspan`; `LayoutInput` gains `col_span`/`row_span`, filled from the node by the
   two builders of a layout tree and folded into the layout signature.
4. **Collapsed borders overlap, they are not repainted.** `border-collapse: collapse` overlaps
   adjacent cells by the narrower facing border so the wider one shows (design note section 4). Paint
   is unchanged. Exact for the common uniform border; per-edge conflict resolution by style and colour
   is not modelled (a named follow-up, not a silent approximation).
5. **Four properties and four roles.** `border-collapse`, `border-spacing`, `caption-side` (inherited)
   and `table-layout` join the registry, the schema, the layout digest and the property status table;
   `table`, `row`, `cell` and `columnheader` join the role tokens and `Role`, with AT-SPI, UIA and
   Android mappings.

## Consequences

- A port with `<table>` compiles and lays out like a browser for auto and fixed layout, spans,
  captions, groups, spacing and collapse; the tags move from `planned` to `mapped` in one change.
- The engine is single-threaded and allocates per layout (a model of rows and cells), like the grid
  engine; it runs only in the measure callback, never on the paint path.
- A `display: table-*` box outside a table is a block box, not wrapped in an anonymous table (CSS
  17.2.1); the common tag use never produces that, and it is listed in the design note.
- The caption sits inside the table box's border and padding rather than outside the border as in
  CSS, because the table is one Yoga leaf. The user-agent sheet gives the table no border, so only an
  author-styled border on a captioned table differs.

## Alternatives considered

- **A new `Table` intrinsic and scene element.** Rejected: it would duplicate `View` (style, events,
  accessibility, hit testing, paint) for no gain, and every pass that handles a node kind (codegen,
  IR validation, designer) would grow a case. Display values already carry the structure.
- **Build tables on `display: grid`.** Rejected: a grid sizes a track from items placed in it, but a
  table column is sized from cells of many rows with spans and percentage and fixed widths, and rows
  from cells that span them. Mapping that onto `grid-template-*` would need the same algorithm in the
  lowering and would not survive an author's `display` change.
- **Approximate first (flex rows).** Rejected by the owner (ADR-059 answer 3).

## Follow-up

- Per-edge border conflict resolution (style and colour priority) and table-owned collapsed borders
  painted once; a caption outside the table border; the anonymous table wrapper for stray
  `table-*` boxes; `rowheader`/`scope` and `rowgroup` roles. Tracked in #1049.
