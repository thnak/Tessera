# List markers: `list-style-position: inside`

Design note for the first-line inline-start box that `list-style-position: inside` needs (issue
#1004, split from #994). It extends [`list-style.md`](list-style.md); the size-runs half of the same
prerequisite is [`inline-size-runs.md`](inline-size-runs.md).

## 1. Problem

An outside marker is painted beside the item and changes no layout. An inside marker is part of the
first line: the item's text starts after it and the line wraps with less room. `li` is a `View`
(flex column), so the first line belongs to a descendant `Text`, and the engine had neither a
first-line box in the wrap nor a way for the item to reach that Text.

## 2. Decisions

- **The box lives in the wrap.** `text::TextStyle::first_line_indent` (px) is the inline-start box of
  the first line of a Text. `wrap_with` (`text_wrap.cpp`) gives the first line `max_width - indent`
  and records it as `TextLine::indent`, included in `TextLine::width`, so a box wider than the line
  never changes the other lines. A hard newline starts a line without it. The field is not read from a
  computed style: the marker is its only source (a general `text-indent` is a separate feature).
  Every cache that keys a wrap carries it: `TextMeasureCache`, `TextLayoutCache`, the layout input
  signature and the retained layout node.
- **Which Text.** The first descendant `Text` of the item in tree order, not entering a nested
  `listitem` or `list` (`first_item_text`, `include/tessera/ui/paint/list_marker_inside.hpp`).
  `marker_host_item` answers the question from the Text's side: its nearest `listitem` ancestor, when
  it is that item's first Text. An item with no Text of its own keeps the outside marker, so
  `inside` never loses a marker.
- **One resolver for layout and paint.** `inside_marker` returns the item, its computed style, type,
  counter and the box width. Bullets are font-relative (`0.3125 + 0.55` em, the outside shape plus its
  gap). A counter is the advance of its text (`1. `, literal, no trimming) through the caller's advance
  seam: layout passes its `TextMeasurer`, paint its `PaintTextMetricsSource`, the same pair the wrap
  already uses for every run.
- **Layout.** `fill_layout_input` (`host_layout_gate.cpp`) sets `LayoutInput::text_indent` from it;
  the measure callback, the text-fit fact and the retained tree read it. `list-style-position` and
  `list-style-type` are layout properties now (`layout_digest`, `is_layout_property`), so changing
  them dirties layout and the layout-unchanged gate cannot skip the pass.
- **Paint.** The item paints nothing for an inside marker. The hosting Text sets the indent on its
  paint `TextStyle` and calls `paint_inside_marker`, which finds the first line's start (alignment
  included), baseline and height from the same layout and calls `draw_list_marker`, the routine the
  outside marker uses, with the right edge at the end of the box. Glyph emission starts `indent` after
  the line origin in every path (`emit_line_runs`, the single-run fast path, spans); text rules skip
  the box. The Text's paint cache key carries the item's counter.

## 3. Limits

- `list-style-image` (`url()`) is still rejected: it needs an image resolved into the box. Tracked in
  a follow-up.
- A marker box wider than the room left on the first line is not wrapped around: the first word
  overhangs. Editable Text (`TextInput`, a Text with a decoration) is not indented.
- Caret and selection geometry, `text-overflow: ellipsis` on a first line narrower than its box, and
  right-to-left items ignore the box beyond what the shared wrap gives them; list items are not
  editable.
- A counter marker's width is measured by layout's measurer and re-measured by paint's source; they
  agree for the stub and for the FreeType path, as for any other run.

Tests: `tests/unit/list_style_tests.cpp` (`list_style`: grammar, shorthand, `@supports`, dirty domain),
`tests/unit/list_style_inside_tests.cpp` (`list_style_inside`: bullets and counters in the box, the
fallback, the resolver, the wrap) and `tests/unit/list_inside_host_tests.cpp` (`list_inside_host`: layout
reserves the box and a position flip reflows through the layout gate).
