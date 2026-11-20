# User-agent audit: every element against a browser

[ADR-063](../../decisions/ADR-063-browser-matching-user-agent-defaults.md) says an unstyled element looks like it does in a browser, and every difference is either fixed or a named exception. This page is the record of that audit (issue #606): how it is measured, what it found, what is fixed, and the exceptions with their reasons.

## Method

`scripts/ua_audit/run.mjs` renders one markup per element twice and compares the boxes:

- **Chromium** (Playwright): the element as HTML in a `display: flow-root` wrapper, so margins stay inside it and read as the element's offset. `line-height: 1.2` and the engine's test font are forced, because line height is a font metric, not a user-agent default.
- **Tessera**: the same markup as TSX through `tessera_scene_preview --scene-json`, with an author stylesheet that is empty, so the preview's fallback sheet does not apply and only the user-agent sheet is measured.

The cases are in `scripts/ua_audit/cases.json`. A row is `same` when the wrapper height (the space the element takes in flow) matches within 0.5px, and, for a block, the element's own box as well. For an inline-block control the control's own box is compared, since the line box's descender gap cannot exist in a flex column.

```text
node scripts/ua_audit/run.mjs --chrome <chrome.exe> --preview build/ninja-msvc/src/compiler/codegen/tessera_scene_preview.exe
```

Playwright is not a repository dependency (set `NODE_PATH` to a directory that has it). Text ink and per-state looks are measured by the pixel method used for #602; this harness covers geometry, which is where most of the gaps were.

## Results

Box size at a 400px viewport, 16px root, before and after this change.

| Element | Chromium | Before | After |
|---|---|---|---|
| `div`, `section`, `nav` (View) | block, full width | flex **row**: children side by side, shrunk to 32px | same |
| `h1` | 32px bold, margin 21.44px | 16px, no margin | same |
| `h2` ... `h6` | 24, 18.72, 16, 13.28, 10.72px; margins 19.92 ... 24.98px | 16px, no margin | same |
| `ul`, `ol` | margin 16px, padding-left 40px | no margin, no indent | same (marker: exception) |
| `a` | blue, underlined | plain text | colour and underline |
| `input` | 176 x 22, 1px border, 13.33px font | **0 x 0** | same |
| `textarea` | 161 x 38 | **0 x 0** | same |
| `button` | height 22, 13.33px font, grey face | 25px, no face | same height, face and hover/active |
| `p` | margin 16px 0 | no margin | same (`role="paragraph"`) |

"Before" for a project that has any author CSS: the preview's fallback sheet hid most of these for a project with none.

## Fixed

- `View { display: block; }`: a `div` is block, not a flex row.
- `h1`-`h6`: bold, size and margin per level. The level reaches the style engine as `aria-level`, which the alias now injects ([`html-aliases.md`](../specs/jsx-intrinsics-v1/html-aliases.md)), and the style tree exposes `role` and `aria-*` as attributes, so `[role="list"]` and `[aria-level="2"]` select as in a browser.
- `p`: the alias injects `role="paragraph"` (a new role token, a11y role and AT-SPI/UIA mapping), so `p` is told apart from `span` and gets the 16px margin.
- `ul`/`ol` margin and indent, links blue and underlined.
- `Button`, `TextInput`, `TextArea`: a size, a 1px border, radius 2px, the 13.33px control font, black on a light face, and the Chrome hover and active colours for `Button`. They are `align-self: flex-start`, so an inline-block control in a block parent takes its own width instead of the full row.

## Open (not fixed, with a reason)

| Gap | Why it is open |
|---|---|
| `em` is 16px, not the element's font size | An engine bug, not a default: #608. The heading margins are written in px until it lands. |
| `:focus-visible` ring, `:disabled` look, placeholder and selection colours | Not measured yet; needs the per-state pixel method. |

## Exceptions (impossible or deliberately different)

| Difference | Reason |
|---|---|
| Adjacent block margins do not collapse | Layout is a flex column per container; margin collapsing is a block-formatting-context rule with no equivalent. Two stacked headings sit further apart than in a browser. |
| `ul`/`ol` draw no marker, `ol` no numbering | No list-item painting yet; the marker needs a `::marker` box. |
| No inline boxes: `span`, `a`, `strong`, `em`, `code` are blocks | Text flows as one block per element, so two adjacent inline elements stack. Inline formatting is a separate feature. |
| `code` is not monospace | The V1 font stack has one face; a monospace face is a font-bundling decision. |
| Two inline-block controls side by side stack | Same inline-flow limit: a column cannot place them on one line. |
| Colour scheme | The UA colours are Chrome's light-scheme values (black on light, link `#0000ee`); a dark scheme needs `color-scheme` support. |
| Text width of controls | The width of a button follows the text measurer; the harness uses the test font, so widths differ by font and are not compared. |

## Adding a rule

Find the difference with the harness, add the rule to `ua_stylesheet_text()` with a comment naming the browser behaviour, pin it in `tests/unit/ua_stylesheet_tests.cpp`, and move the row from Open to Fixed here. A difference with no rule and no row above is a bug.
