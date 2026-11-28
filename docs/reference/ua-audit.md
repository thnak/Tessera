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
- `ul`/`ol` margin and indent, links blue and underlined; list markers (`disc`/`circle`/`square` by depth, `decimal` for `ol`) drawn by the items (issue #541).
- Canvas background: the window root has no background of its own, so the app root view's opaque `background-color` is laid over the whole window first (`PaintContext::canvas`, `canvas_background`), as a browser propagates the root/`body` background to the canvas. The view keeps its laid-out box; a translucent colour is not propagated (it would blend twice), and the root's own background wins. The host latches a full redraw when the colour changes.
- `pre`, `blockquote`, `hr` (issue #541): `pre` is a block with a 1em vertical margin (its monospace face and `white-space: pre` are the alias's own defaults), `blockquote` has `margin: 1em 40px`, and `hr` is a zero-height box with a 1px dark top and light bottom edge (Chrome's `inset` rule) and a 0.5em margin. They select by the tag the alias keeps, as they have no role.
- `Button`, `TextInput`, `TextArea`: a size, a 1px border, radius 2px, the 13.33px control font, black on a light face, and the Chrome hover and active colours for `Button`. `Button` shrinks to its own width in a block parent (inline-block) but stretches as a flex item of a flex container, as in Chrome: its `align-self` is a marker the layout resolves per parent (issue #1018). `TextInput`/`TextArea` use `min-width`/`min-height` (border-box 176x22, 161x38, written content-box: 168x16, 153x32; the fields are content-box as in Chrome, issue #973) and stay stretchable, so a field in a flex column fills it as in a browser; the one difference is that a bare input in a block parent fills the row instead of 176px (a named exception below).

- States: `Button:disabled`, `TextInput:disabled`, `TextArea:disabled` fade to Chrome's 30% face, border and text (after hover/active, so a disabled button does not react); `:focus-visible` draws Chrome's 2px #101010 ring on the three controls as a zero-blur `box-shadow` spread, since the engine has no `outline`.

## Open (not fixed, with a reason)

| Gap | Why it is open |
|---|---|

| Placeholder and selection colours | Not measured yet; needs the per-state pixel method and typed input in the Studio (#601). |
| Pixel-measured per-state look against Chromium | The rules below take Chrome's UA values from its sheet, not from a pixel diff; the per-state ink measurement of #602 still has to confirm them. |

## Exceptions (impossible or deliberately different)

| Difference | Reason |
|---|---|
| Adjacent block margins do not collapse | Layout is a flex column per container; margin collapsing is a block-formatting-context rule with no equivalent. Two stacked headings sit further apart than in a browser. |
| `ol` ignores `start`/`reversed`; markers are always outside and on the left | `list-style-position: inside`, `::marker` and counters are not modelled ([`list-style.md`](../design/list-style.md)). |
| No inline boxes: `span`, `a`, `strong`, `em`, `code` are blocks | Text flows as one block per element, so two adjacent inline elements stack. Inline formatting is a separate feature. |
| `code`, `kbd` and `pre` are not monospace | The V1 font stack has one face; a monospace face is a font-bundling decision. |
| Two inline-block controls side by side stack | Same inline-flow limit: a column cannot place them on one line. |
| `outline` / `outline-offset` on the focus ring | The engine has no `outline` property; the ring is a 2px `box-shadow` spread (same pixels at `outline-offset: 0`, but it follows the radius, and an author `box-shadow` replaces it). |
| Colour scheme | The UA colours are Chrome's light-scheme values (black on light, link `#0000ee`); a dark scheme needs `color-scheme` support. |
| A bare `input`/`textarea` in a block parent fills the row | A browser sizes an inline-block input to 176px; here a block is a stretching column, and a fixed width would stop fields stretching in flex layouts (the gallery's form). The audit ignores `width` for these two. |
| Text width of controls | The width of a button follows the text measurer; the harness uses the test font, so widths differ by font and are not compared. |

## Page parity (the showcase sample)

`scripts/ua_audit/page.mjs` compares a whole sample with its browser twin (`tools/parity/showcase.html`, no reset, the sample's own stylesheets) element by element at two widths. First run on `samples/showcase` found:

| Finding | Status |
|---|---|
| Controls inherited `line-height` (button 42.5px instead of 40px) | fixed: `Button, TextInput, TextArea { line-height: normal }` |
| Author `h1 { ... }`, `h3 { ... }`, `a { ... }` never match: the tag is not kept | fixed (#610): an alias keeps its tag as `tag`, which a type selector matches |
| `max-width` + `margin: 0 auto` on the page root: 866px off-centre at 900, 426px at 400 | fixed (#611): a block child with `width: auto` and auto margins fills up to `max-width` and centres, root included |
| A page taller than the window was squeezed to the window height (a 751px page at 300px: hero 79px instead of 260px) | fixed (#611): a child of `display: block` never shrinks, so content overflows the window and is cut at its edge, as in a browser viewport (scrolling is separate) |
| Root is the window height: taller content shrinks instead of growing | fixed (#611): the root is at least the viewport tall and grows with its content. The area no author paints is the canvas colour, white as a browser's default canvas (it was a navy app background, which showed as a dark strip under a short page); a background on the root paints the whole window. Dark colour scheme still has a white canvas by default: named exception (colour scheme) |
| 8px collapsed margin between `h1` and the next paragraph shows as 16px | named exception (margin collapsing, above) |
| Text width and wrapping | the harness uses the stub measurer, so a wrapped text node differs by line count; reported as `text`, not counted |

### The Studio samples (issue #1018)

Run on the Linux box against Chromium, 900x700 and 400x800, with the twins' body pinned to the stub's
line height of 1.2 (a font metric, not a user-agent default). Demo, Counter and Lists match; Grid
matches apart from text widths; Motion differs only in text-driven widths. Found and fixed:

| Finding | Status |
|---|---|
| A `Button` in a flex column kept the UA `align-self: flex-start` (56px wide where Chrome stretches it to 360px) | fixed: the sheet writes a marker the layout turns into `flex-start` only for a child of `display: block` |
| A grid item with `container-type: inline-size` was 0 wide (the zero written for containment pinned it) | fixed: a grid item is stretched to its area when placed; it still contributes zero to track sizing |
| `1fr` tracks were never narrower than their items' unwrapped text (a card row overflowed, the header was 416px at 400px) | fixed: a container item's min-content is the reach of its descendants laid out at zero width |
| An auto grid row was sized for the unwrapped text, so a wrapped card overflowed its row | fixed: heights are measured again at the resolved column width |
| `@container` never matched in the preview (the layout input used the plain style pass, which takes no container source) or in a one-shot `--scene-json` | fixed: the preview pass is memoised against the settle loop's source, and a one-shot render settles once |

Named exceptions (not counted by the harness): text width and wrapping come from the stub measurer
(a link or heading is a few pixels narrower or wider than Chromium's real face; the Motion drawer, sized by its
paragraph, follows), and the Button/TextInput caption sits in a child the harness skips.

## Adding a rule

Find the difference with the harness, add the rule to `ua_stylesheet_text()` with a comment naming the browser behaviour, pin it in `tests/unit/ua_stylesheet_tests.cpp`, and move the row from Open to Fixed here. A difference with no rule and no row above is a bug.
