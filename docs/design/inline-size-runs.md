# Inline size runs: `sub`, `sup` and `small` inside a `Text`

Design note for issue #1004 (split from #994). Normative span rules:
[`elements-basic.md`](../specs/jsx-intrinsics-v1/elements-basic.md) section 2.1.

## 1. What changed

A `Text` with inline spans is one measured paragraph. Until now every byte of it was measured at the
paragraph's font size, so a span with a smaller `font-size` (the `sub`, `sup` and `small` defaults,
`0.83em`) was shifted but not shrunk. The paragraph now carries a **size run table**: byte ranges that
are measured and drawn at a fraction of the paragraph size.

- `TextSizeRun {begin, end, scale}` and the interned `TextSizeRunsId` live in
  `include/tessera/ui/text/size_runs.hpp`. The id is a 4-byte `TextStyle::size_runs` field, interned like
  `FontFeaturesId` (256 distinct tables, then the spans keep the paragraph size), so the existing
  measure/layout seams and cache keys carry it without a signature change.
- The wrap (`text_wrap.cpp`) measures every run it places piecewise: a segment that crosses a span
  boundary is split at the boundary and each piece is measured at `font_size * scale`. The literal
  inter-word gap stays at the paragraph size. Line breaks, widths and the paint advances therefore agree.
- Both passes build the table from the spans' computed font size (`size_runs_from_spans`): layout in
  `fill_layout_input` (`LayoutInput::text_runs`, part of the layout signature and the retained text
  change check), paint in `paint_spanned_text`. Paint draws each slice at the span size and advances
  it with that size.

## 2. Rules and limits

- **Shrink only.** A span whose computed size is not below the paragraph's is ignored (scale below 1
  only, floor 0.25). The line box, the baseline and the line height stay the paragraph's, so a larger
  span would overlap its neighbours; growing a line box needs per-line metrics and is not modelled.
- **Source of the size.** The span's computed `font-size` (alias default, `className` or the stylesheet).
  An author's inline `style` size on a span stays `TESSERA_JSX_SPAN_METRIC`. `letter-spacing` and
  `white-space` on a span remain the paragraph's.
- **`vertical-align`** still shifts by the paragraph's size (`sub` 0.2 em down, `super` 0.4 em up) and
  does not change the line box.
- **Caret and selection.** Spans exist only in a non-editable `Text`; the editing paths do not read the
  table.
- **Cache.** The measure cache key includes the id. `TextLayoutCache` is not used by the spanned path,
  so it does not carry it.

Tests: `tests/unit/text_size_runs_tests.cpp` (`text_size_runs`), `test_shrunk_span` in
`tests/unit/paint_text_span_tests.cpp`.

## 3. The first-line inline-start box

`list-style-position: inside` needed a box at the start of the first line; it is
`TextStyle::first_line_indent` and the item-to-first-Text resolver, in
[`list-style-inside.md`](list-style-inside.md). `list-style: url()` is still rejected
([`list-style.md`](list-style.md) section 3).
