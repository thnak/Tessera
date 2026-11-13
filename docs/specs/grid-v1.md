# CSS Grid subset (V1)

Status: implemented for Big Step B2 (issue #249). This is the normative value list for the grid
subset that [`stylesheet-v1.md`](stylesheet-v1.md) section 11 (compatibility matrix, **Grid** row)
names. Rationale, the engine seam and the nesting story are in
[`layout.md`](../design/layout.md); the per-property consumer record is in
[`stylesheet-property-status-v1.md`](stylesheet-property-status-v1.md).

The engine is a C++ `GridEngine` under `src/ui/layout/` behind the layout seam
(`ui/layout/layout_engine.hpp`). UI code never includes it: `LayoutEngine` is the only caller.
There is no new dependency (no taffy, no Rust toolchain).

## 1. Properties and supported values

| Property | Supported values | Initial |
|---|---|---|
| `display` | `grid`, `inline-grid` (both block-level in V1: there is no inline formatting context) | `inline` |
| `grid-template-columns`, `grid-template-rows` | `none`; a whitespace-separated track list of `<length>`, `<percentage>`, `<flex>` (`<number>fr`), `auto`, `min-content`, `max-content`, `minmax(<min>, <max>)` and `repeat(<integer>, <track-list>)` | `none` |
| `grid-column`, `grid-row` | `auto`, `<integer>` (1-based line, negative counts back from the explicit grid's last line), `span <integer>`, and the two-value form `<grid-line> / <grid-line>` | `auto` |
| `grid-auto-flow` | `row`, `column` | `row` |
| `justify-items`, `align-items` | `start`, `end`, `center`, `stretch`, `normal`, `left`, `right`, `flex-start`, `flex-end`, `legacy`, `baseline` (`baseline` behaves as `start` for a grid item) | `legacy` / `stretch` |
| `justify-self`, `align-self` | the same set plus `auto` (inherits the container's items value) | `auto` |
| `justify-content`, `align-content` | `start`, `end`, `center`, `stretch`, `normal`, `flex-start`, `flex-end`, `left`, `right`, `space-between`, `space-around`, `space-evenly` | `normal` |
| `row-gap`, `column-gap`, `gap` | `<length>` (the stylesheet engine's unit set: `px`, `rem`/`em`, `vw`, `vh`, `pt`) or `<percentage>`; `normal` means no gap. `gap` is the `row-gap`/`column-gap` pair shorthand. | `normal` |

`grid-column` and `grid-row` are registered as **longhands** that carry the whole `<start> / <end>`
value. V1 has no `grid-column-start`/`grid-column-end`/`grid-row-start`/`grid-row-end` longhands and
no `grid-area`/`grid-template-areas`/`grid`/`grid-template` shorthands.

`minmax()` is supported with a non-flexible minimum: `minmax(<length-percentage> | min-content |
max-content | auto, <length-percentage> | <flex> | min-content | max-content | auto)`. A `<flex>`
minimum (`minmax(1fr, 2fr)`) is invalid, as in CSS.

A value outside this grammar is **never silent**: the declaration validator drops it and reports
`UnsupportedValue` with a message that names the unsupported forms, and `@supports` answers `false`
for it. The same parser serves the validator, `@supports` and the layout engine
(`ui/style/grid_values.hpp`), so the three cannot disagree.

## 2. Track sizing

The algorithm is a documented subset of CSS Grid section 12:

1. Fixed and percentage tracks resolve immediately (a percentage resolves against the container's
   content box on that axis; a percentage in an indefinite container behaves as `auto`).
2. Content-sized tracks (`auto`, `min-content`, `max-content`, and the implicit tracks that
   auto-placement creates) take the largest max-content contribution of the items in them. An item
   spanning several tracks distributes the deficit it adds equally across the tracks it spans.
   A `min-content` track is sized to the contribution as well: V1 has no separate min-content pass
   for containers, so a content-sized track never shrinks below its item's unwrapped size. A text
   leaf is measured at zero width, which does report its min-content width.
3. Free space then grows the non-flexible tracks up to their growth limits (`minmax()` maxima).
4. Remaining free space is shared by the `fr` tracks in proportion to their flex factors. If the
   free space is negative, an `fr` track keeps its base size and the grid overflows.
5. When the leftover space is still positive and `align-content`/`justify-content` resolves to
   `normal`/`stretch` (the CSS default), the `auto` tracks stretch equally to fill the container.
   Otherwise the track grid is aligned in the container by `align-content`/`justify-content`.

Gaps are subtracted before `fr` distribution and counted between spanned tracks.

## 3. Placement

Items are placed in `order`-modified document order (the same stable `order` sort the flex adapter
uses), then:

1. An item with both axes definite is placed at its lines.
2. Items locked to one axis fill the first free position on the other axis.
3. The remaining items are auto-placed with a sparse cursor in the `grid-auto-flow` direction
   (`row`: fill a row, then the next row; `column`: fill a column, then the next column). The cursor
   never moves backwards, so there is no dense packing.
4. Implicit tracks created outside the explicit template are `auto`; V1 has no
   `grid-auto-columns`/`grid-auto-rows`.

An item whose lines resolve below 1 is clamped to the first track. `span` values and line numbers
are capped at 100000; a template is capped at 1000 tracks (including a `repeat()` expansion), which
bounds the memory a hostile stylesheet can request.

## 4. Alignment

`justify-items`/`align-items` on the container supply the default, and `justify-self`/`align-self`
on the item override it (`auto` inherits). `stretch` fills the grid area; `start`/`end`/`center`
size the item to its max-content size, clamped to the area, and position it there. `left`/`right`
are physical; `start`/`end` mirror with `direction: rtl`. Margins are subtracted from the area, and
the border box is positioned inside what remains.

## 5. Direction

`direction: rtl` mirrors the column order: track 1 starts at the container's right edge, and the
logical `start`/`end` alignment values swap. `writing-mode` is not a layout input: V1 is
horizontal-only (see [`text.md`](../design/text.md)).

## 6. Nesting and the layout seam

A grid container is a Yoga leaf whose measure callback reports the grid's content size, so a grid
inside a flex container is measured and sized like any other flex item. Each grid item's subtree is
then laid out as an independent Yoga root at its grid-area size, so a flex container inside a grid,
text measurement, and the text measure cache all work unchanged. Grid inside grid works by
recursion: the inner grid container is again a measured leaf of the item subtree. An inner grid's
contribution to an outer content-sized track is measured from its own content size, so give the
inner grid an explicit size (or keep the outer tracks fixed/`fr`) when it must not be measured.

## 7. Out of scope for V1

Subgrid, masonry, dense auto-placement, named lines and named areas, `grid-auto-columns`/
`grid-auto-rows`, the `grid`/`grid-template`/`grid-area` shorthands, `fit-content()`, and
`baseline` alignment (accepted and treated as `start`). `minmax()` with a flexible minimum is
invalid. Absolutely positioned children of a grid container are placed like normal items (they are
not taken out of flow).

## 8. Performance

A 1,000-item grid (`repeat(10, 1fr)` columns, 100 fixed rows) lays out in **15 ms** (measured range
14–24 ms over five runs on the reference machine of the #241 CPU benchmark,
[`cpu_benchmark.txt`](../../tests/golden/perf/cpu_benchmark.txt)), far inside that benchmark's
4500 ms first style+layout budget for 12 000 nodes. The measured number is printed by `grid_layout`
(`ctest -R grid_layout -V`). A grid with fixed or `fr` tracks performs one Yoga layout per item;
content-sized tracks add one max-content measure per item.
