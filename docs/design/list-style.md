# List markers: `list-style`

Design note for `list-style-type`, `list-style-position` and the `list-style` shorthand, and for the
markers of `ul`/`ol`/`li` ([ADR-059](../../decisions/ADR-059-html-tags-supported-not-rejected.md)
group A, issue #541). Normative property rows:
[`stylesheet-property-status-v1.md`](../specs/stylesheet-property-status-v1.md).

## 1. Properties and grammar

One grammar (`include/tessera/ui/style/list_style.hpp`), shared by the registry validator, `@supports`,
the shorthand and the painter.

| Property | Accepted | Rejected (declaration dropped, diagnostic reported) |
|---|---|---|
| `list-style-type` | `none`, `disc`, `circle`, `square`, `decimal`, `lower-alpha` (`lower-latin`), `upper-alpha` (`upper-latin`), `lower-roman`, `upper-roman` | every other counter style (`georgian`, `cjk-*`, `decimal-leading-zero`), a quoted string, `symbols()` |
| `list-style-position` | `outside` | `inside` |

`list-style` is `<type> || <position>` or `none`. It expands to the two longhands and resets the omitted
one to its initial value; `url()` (`list-style-image`), `inside`, a repeated component or an unknown one
drops the whole shorthand, never half of it. Rejected values are the ones the engine cannot draw
faithfully; they are reported, not approximated.

Both longhands are inherited, so a rule on `ul` reaches its `li` children. The initial type is `none`,
not CSS's `disc`: the engine has no `display: list-item`, so a marker exists only where a rule asks for
one. The user-agent sheet gives `ul`/`ol` their type (Chrome's: `disc`, `circle` for a list inside a
list, `square` for two levels deep, `decimal` for `ol`), selected through the tag the alias keeps. A
custom list written as `role="list"`/`role="listitem"` Views stays bare unless its author sets a type.

## 2. Paint

Both properties are paint-only: a marker sits outside the item's border box, so it changes no layout
and a change marks paint dirty only (`paint_list_marker.cpp`).

- **Which nodes.** A node with `role="listitem"` (the `li` alias injects it) whose computed type is not
  `none`. The counter is the 1-based position among the `listitem` children of its parent, so a sibling
  that is not a list item is not counted. The list's `aria-start`, `aria-reversed` and an item's
  `aria-value` (the targets of `ol start`, `reversed` and `li value`,
  [`html-aliases.md`](../specs/jsx-intrinsics-v1/html-aliases.md) section 3.7) number it as HTML does: the
  counter starts at `start` (1, or the item count when reversed), a `value` resets it for that item and the
  rest, and it moves by +1 or -1 (reversed). It may be zero or negative; alphabetic and roman types fall
  back to decimal below 1.
- **Shapes.** `disc`, `circle` and `square` are geometry (a rounded rect, a ring, a rect), 0.3125 em wide,
  with the right edge 0.55 em before the item and the centre 0.35 em above the first baseline: the
  measurements of Chrome's disc at 16 px. Geometry rather than a glyph, because the app face has no
  guarantee of U+25E6 or U+25AA.
- **Counters.** The counter text and its `. ` suffix (`1. `, `b. `, `iv. `) are drawn as one text run
  whose right edge is the item's left edge, in the item's font and colour. Alphabetic types use
  bijective base 26 (`aa` after `z`); roman stops at 3999 and, like alphabetic zero, falls back to decimal.
- **First line.** The marker is aligned to the first line of the item's own text style: the line box
  and baseline of one measured line at the item's font, below its border and padding.
- **Cache.** The marker depends on siblings, not on the item, so the list position is part of the paint
  cache key (`PaintCacheSignature::list_ordinal`); inserting an item before another repaints it.

## 3. Limits

- `list-style-position: inside` and `list-style-image` are rejected: flowing a marker into the first
  line needs inline layout the engine does not have.
- `sub`/`sup`/`small` inside a `Text` shrink through size runs ([`inline-size-runs.md`](inline-size-runs.md)); the first-line marker box that `inside` needs is the part still missing (section 3 there).
- No `::marker` pseudo-element, no `counter()`, no `@counter-style`; `display: list-item` does not exist.
- Right-to-left lists draw the marker on the left; the logical (start-side) placement is not modelled.

Tests: `tests/unit/list_style_tests.cpp` (`list_style`). Goldens: the gallery `lists` page (`samples/gallery/ListsPage.tsx`, `tests/golden/gallery/lists_light.png` and `lists_dark.png`) pins nested bullets, decimal, `start`/`reversed`/`value`, roman and alphabetic markers and a `sub`/`sup`/`small` line; lavapipe images only (`gallery_golden_lists_*`).
