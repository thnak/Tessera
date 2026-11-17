# Comparing the engine with Chrome

Status: developer tool (V1-M4, not a CI gate). It answers one question: for the same markup and the
same stylesheet, how far is what the engine paints from what a browser paints?

## 1. What is compared

| Side | Source | Run by |
|---|---|---|
| Engine | `samples/gallery/FormPage.tsx` painted by `gallery --page form --capture` (lavapipe, so the pixels are deterministic) | the gallery sample |
| Chrome | `tools/parity/form.html`, the same structure as HTML | `scripts/ci/chrome_parity.mjs` |

Both read the same files: `packages/tessera-ui/button.css`, `samples/gallery/form.css`, the theme
tokens of `instrument.theme.json` (written into the page as `:root` variables) and the font
`tests/fixtures/fonts/NotoSans-Regular.ttf`. A difference is therefore a difference in how the two
renderers read the same rules, not a difference in the rules. `FormPage.tsx` is deliberately busy: a
card with a shadow, a two-column grid with a spanning item, text inputs (one in an error state), a
select drawn from views, a segmented control, switches and a checkbox, a truncated help line, and a
footer of buttons in several variants and states.

## 2. Run it

```bash
# engine (Linux with Mesa lavapipe; the capture is 640x720)
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  build/dev/samples/gallery/gallery --page form --capture out/parity/engine.png --width 640 --height 720

# Chrome (playwright is not a repository dependency: npm install playwright)
node scripts/ci/chrome_parity.mjs --out out/parity [--chrome <path to chrome>]

# compare
python scripts/ci/chrome_parity_compare.py --dir out/parity
```

The comparison writes `side_by_side.png`, `diff.png`, `overlay.png` (Chrome's element boxes drawn over
the engine capture, so a layout difference reads as an outline that does not hug its element) and
`report.json`.

## 3. Reading the numbers

| Number | Meaning |
|---|---|
| `differing_pixels_pct` | Share of pixels that differ by more than the threshold. Glyph edges always differ, so a floor of a few percent is text anti-aliasing, not a bug |
| `structure_diff_pct` | The same comparison on blurred, downscaled copies. It ignores edge filtering and measures where things are; read this one first |

On the parity form the structure difference is **0.12 %** and the pixel difference about **4 %**,
which is text rendering: the engine's glyphs are lighter than Chrome's.

## 4. Where the two models differ on purpose

These are not bugs; a markup author who writes for both has to know them.

- **A component instance is a host node.** Each `Component` is built as a generated host node around
  the callee's root ([ir-v1-expr.md](../specs/ir-v1-expr.md), the `Component` row), so
  `.row > .btn` does not match a `Button` whose root has `btn`: the host is the child. Use `.row > *`
  (as `gallery.css` does) or a descendant selector. `tools/parity/form.html` wraps each library
  Button in a `host` element to model this.
- **Box model.** `width` and `height` are `content-box`, the CSS default, as in a browser. The
  engine does not apply a border-box reset.
- **View defaults.** Every `View` is a flex container laid out by Yoga (column, stretch, no shrink);
  `tools/parity/form.html` carries the equivalent reset.

## 5. Differences this tool found and that were fixed

| Difference | Fix |
|---|---|
| An item with `grid-column: 1 / 3` was placed before the items ahead of it in the document | `grid_placement.cpp` places an item with a definite line on the flow's own axis in document order ([grid-v1.md](../specs/grid-v1.md) section 3) |
| A border ignored `border-radius`: a rounded background inside a square outline | a uniform border on a rounded box is one ring: `PaintSink::fill_rounded_ring`, `Batched2DRenderer::push_rounded_ring`, and a ring mode (`mode = 3 + width`) in `batched2d.frag` and `batched2d.wgsl` that subtracts the inner rounded box |
| An explicit `line-height` did not move the glyphs: the baseline was always the font's ascent, so a mark in a 14 px line around a 16 px font sat about 2 px low | `text_wrap.cpp` puts the baseline at `ascent + (line-height - (ascent + descent)) / 2` and the single-line paint path passes it to the sink ([text.md](../design/text.md)); the worst remaining text offset on the form is 1 px |
| Text in a single-line `TextInput` sat at the top of the field | `paint_text.cpp` centres the line in the content box ([text-editing-v1.md](../specs/text-editing-v1.md)) |

## 6. Known limits

- A border with different widths or colours per side is not one ring and keeps the square edge strips
  on a rounded box.
- The font has one weight, so bold is not compared. Only the light theme at device scale 1 is
  covered; dark, high contrast, RTL and scale 2 need their own runs.
- The engine's element boxes are not exported, so layout is compared through the overlay, not as
  numbers.
