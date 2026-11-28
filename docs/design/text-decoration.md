# Text decoration and vertical alignment

Design note for `text-decoration` (underline, overline, line-through) and `vertical-align`
(baseline shift). Normative property rows: [`stylesheet-property-status-v1.md`](../specs/stylesheet-property-status-v1.md).
The HTML tags that use them (`u`, `s`, `sub`, `sup`, `mark`, ...) are group A of
[ADR-059](../../decisions/ADR-059-html-tags-supported-not-rejected.md) and map through the alias
defaults of [`html-aliases.md`](../specs/jsx-intrinsics-v1/html-aliases.md) section 2 (issue #541).
Inside a `Text` a span keeps the parent's `font-size`, so `sub`, `sup` and `small` shift or decorate
but do not shrink there (the rows are `partial` for that reason).

## 1. Properties and grammar

One grammar, shared by the registry validator, `@supports`, the shorthand and `text_style_from`
(`include/tessera/ui/style/text_decoration.hpp`).

| Property | Accepted | Rejected (declaration dropped, diagnostic reported) |
|---|---|---|
| `text-decoration-line` | `none`, or any set of `underline`, `overline`, `line-through` | repeats, `none` combined, `blink`, `spelling-error`, `grammar-error` |
| `text-decoration-style` | `solid` | `double`, `dotted`, `dashed`, `wavy` |
| `text-decoration-color` | `currentColor` or any colour | |
| `text-decoration-thickness` | `auto`, `from-font`, a non-negative px/em/rem length | percentages, negatives |
| `vertical-align` | `baseline`, `sub`, `super`, a signed px/em/rem length; `top`, `middle`, `bottom` (shift no text; they place a table cell's content, [`table-layout.md`](table-layout.md)) | `text-top`, `text-bottom`, percentages |

`text-decoration` is the shorthand `<line> || <style> || <color> || <thickness>`. It expands to the
four longhands and resets the omitted ones to their initial values. An unsupported component drops
the whole shorthand, never half of it. Rejected values are the ones the engine cannot draw
faithfully; they are reported, not approximated.

## 2. Paint

Both properties are paint-only. They change no glyph advance, line box, baseline or node size, so
they are not part of the measure or layout cache keys and a change marks only paint dirty.

- **Rules** are rects emitted through `PaintSink::fill_rect` after the glyph runs they decorate
  (`paint_text_rules.cpp`). Per line the rule spans the laid-out line width, clipped to the content
  box. Edges are rounded to whole pixels so rules of adjacent runs abut.
  - thickness: the declared length, else `max(1px, round(font_size / 16))`;
  - overline: top at the font ascent above the baseline;
  - line-through: centred 0.3 em above the baseline;
  - underline: top at `max(1px, 0.4 * descent)` below the baseline;
  - colour: `text-decoration-color`; `currentColor` is the run's text colour.
- **Baseline shift** moves the run and its rules vertically by the shift in px. `sub` lowers by 0.2 em
  and `super` raises by 0.33 em of the computed `font-size` (CSS leaves both UA-defined). The smaller
  size sub/sup text normally has is the stylesheet's `font-size`; the engine never shrinks text on
  its own. The line box is not grown for the shift, so a raised run can paint outside its node box
  (the same way `text-overflow: clip` overflows).

## 3. Propagation and inline spans

In CSS a decoration is drawn by the box that declares it and propagates to its in-flow descendants,
and the colour is the declaring box's. The engine models propagation as inheritance of the four
`text-decoration-*` longhands, so `<u>a <b>b</b></u>` underlines both runs and nested blocks inherit
the line. Two consequences are intentional and documented:

- a descendant that sets `text-decoration-line: none` removes the line for itself (CSS cannot);
- `currentColor` resolves against each descendant's own colour.

`vertical-align` is not inherited. For a `Text` node with inline spans, each span resolves its own
decoration and shift through the normal cascade; unattributed bytes use the parent's. Spans paint at
the parent's font size (the existing span rule), so a span's em-relative shift is taken against that
size.

## 4. Limits

- Editable text (`TextInput`, `TextArea`, the caret/selection path) shifts with `vertical-align` but
  does not draw decoration rules.
- Only solid rules; no `text-underline-offset`, `text-decoration-skip-ink` or per-glyph skipping.
- Decorations are drawn per laid-out line and per span slice; a `text-overflow: ellipsis` line is
  underlined up to the clipped width.

Tests: `tests/unit/text_decoration_tests.cpp` (`text_decoration`).
