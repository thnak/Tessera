# Comparing the engine with Chrome

Status: developer tool (V1-M4, not a CI gate). It answers one question: for the same markup and the
same stylesheet, how far is what the engine paints from what a browser paints?

## 1. What is compared

| Side | Source | Run by |
|---|---|---|
| Engine | a gallery page painted by `gallery --page P --capture` (lavapipe, so the pixels are deterministic) | the gallery sample |
| Chrome | `tools/parity/P.html`, the same structure as HTML | `scripts/ci/chrome_parity.mjs --page P` |

Two pages: `form` (`FormPage.tsx`, `form.module.css`, light theme) and `dash` (`DashPage.tsx`, `dash.css`, run with
`--theme instrument-dark` on both sides).

Both read the same files: `packages/tessera-ui/button.css`, `samples/gallery/form.module.css`, the theme
tokens of `instrument.theme.json` (written into the page as `:root` variables) and the font
`tests/fixtures/fonts/NotoSans-Regular.ttf`. A difference is therefore a difference in how the two
renderers read the same rules, not a difference in the rules. `FormPage.tsx` is deliberately busy: a
card with a shadow, a two-column grid with a spanning item, text inputs (one in an error state), a
select drawn from views, a segmented control, switches and a checkbox, a truncated help line, and a
footer of buttons in several variants and states.

## 2. Run it

One command answers pass or fail (issue #529):

```bash
python -B scripts/ci/parity_gate.py --page form
```

It exits 0 when `structure_diff_pct` is within the threshold (0.5 % by default, the form being at
0.12 %), 1 when it is over, and **2 when a step could not run** - a missing Playwright, a missing
gallery binary, a capture that wrote nothing. A step that cannot run is not a pass. It prints both
numbers and the artifact paths, so a farm agent can attach them to a PR.

`differing_pixels_pct` is printed but **not gated**: it is dominated by text anti-aliasing and would
gate the font rather than the component. `--page dash` works too (plain CSS); `--page form` writes the
module's scoped names first (the form's stylesheet is a CSS module, #463).

### The four steps it runs, by hand

Kept as the fallback for when a step needs watching:

```bash
# engine (Linux with Mesa lavapipe; the capture is 640x720)
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json \
  build/dev/samples/gallery/gallery --page form --capture out/parity/engine.png --width 640 --height 720

# the form's stylesheet is a CSS module (#463): write its scoped CSS and class map for the twin
build/dev/samples/gallery/gallery --emit-module-css out/parity

# Chrome (playwright is not a repository dependency: npm ci --prefix scripts/ci)
node scripts/ci/chrome_parity.mjs --out out/parity [--chrome <path to chrome>]

# compare
python scripts/ci/chrome_parity_compare.py --dir out/parity
```

The comparison writes `side_by_side.png`, `diff.png`, `overlay.png` (Chrome's element boxes drawn over
the engine capture, so a layout difference reads as an outline that does not hug its element) and
`report.json`.

**This is not a CI gate, deliberately.** It needs a browser and a lavapipe capture, so it stays a
developer and farm tool; a lane that ran it on every PR would need Chrome on that lane. The gate's own
logic is tested without a browser (`scripts/ci/parity_gate_tests.py`).

`DashPage.tsx` covers what the form does not: round avatars, a solid hero with a coloured shadow, a rotated
square, `letter-spacing` with `text-transform`, right-aligned numbers, `opacity`, an underline tab and table rows
(per-side `border-bottom`), progress fills clipped to a rounded track, wrapping chips and an absolutely placed dot.

### Animation

The plan control of the form (`Monthly`, `Yearly`, `Invoice`) is interactive and declares a
`background-color` and `color` transition (200 ms, ease). Both sides click the second segment and stop at
the same instant: the engine with `--click seg:1 --advance-ms N` (a manual clock, so the instant is
exact), Chrome with `--click seg:1 --advance-ms N` (it pauses every running transition and sets its
`currentTime`). `tools/parity/form.html` carries the one script that mirrors `FormPage.tsx`'s `onClick`.

```bash
build/dev/samples/gallery/gallery --page form --click seg:1 --advance-ms 100 --capture out/parity/engine.png
node scripts/ci/chrome_parity.mjs --click seg:1 --advance-ms 100 --out out/parity
```

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
| A blurred `box-shadow` was a stack of 3 to 8 concentric fills, so the falloff showed countable steps | `PaintSink::fill_box_shadow` and `Batched2DRenderer::push_soft_shadow`: one quad whose shader (`batched2d.frag`, `batched2d.wgsl`) evaluates `0.5 * (1 - erf(d / (sigma * sqrt 2)))` from the rounded box's signed distance. Below the card the engine's vertical profile is within 3 of 255 levels of Chrome's. The CPU reference sinks keep the stack |
| A shadow was painted *inside* a translucent box, so a transparent box with a shadow looked filled with grey where Chrome shows the page | `paint_box_shadows` hands every layer the node's rounded border box; the one-quad shader subtracts it (`Vertex2D::carve`, `batched2d.frag`, `batched2d.wgsl`) and the flattening sinks emit four strips around it ([paint-pass.md](../design/paint-pass.md) section 5.1). On the same four-case scene the centre of a transparent box is 255 in the engine and in Chrome (178 before), the shadow band below is 235 vs Chrome's 234, and the corner triangle of a rounded box is 245 vs Chrome's 246. The form's unselected segment has a `transparent` base fill again |
| Edges were blended over a 2 px band (`aa_radius` 1.0), so a pixel-aligned edge left its inside pixels 16 % transparent: a 1 px border read lighter than Chrome's and a 2 px ring let the fill under it show through | `Batched2DRenderer` passes 0.5, the exact one-pixel box coverage ([antialiasing.md](../design/antialiasing.md)); pixel difference 4.1 to 3.6 % on the form, 3.3 to 2.9 % on the dashboard |
| Text looked faint and its descenders short: a glyph quad sat on a fractional pixel position and the atlas is sampled with a linear filter, so every row blended with its neighbour | `text_renderer.cpp` snaps each glyph quad to whole pixels (the pen keeps its fractional advance). Dark text ink went from 0.91 to 0.97 of Chrome's to 1.00 to 1.02 |
| Light text on a dark surface carried 20 to 28 % less ink than Chrome's | glyph quads add a coverage boost to the textured mode (`kCoverageBoostScale`, `batched2d.frag`, `batched2d.wgsl`), 1.0 for dark text up to 1.4 for white; ink is now 0.95 to 0.96 of Chrome's |
| A single glyph in a centred box sat a row too low and most of a pixel left of centre (the `x` of the checkbox) | the baseline uses Blink's whole-pixel sum, `round(ascent) + floor((line-height - (round(ascent) + round(descent))) / 2)` (`text_wrap.cpp`); layout runs with Yoga's pixel rounding off (`layout_engine.cpp`), because Yoga floored a measured node's left edge and ceiled its right one. The mark's ink centre is now 0.14 px from the box centre, as in Chrome |
| A transition ticked and never reached the pixels | the window driver and the frame capture each called `paint::paint_scene` and neither set the tick's sampled values as `PaintContext::view`; both now call `AppHost::paint_scene` ([app-host.md](../design/app-host.md)) |
| A running transition never finished (63 % after its full duration) | the style stage retargeted it on every frame, from the displayed value to the unchanged target; only a changed property is retargeted ([animation-runtime.md](../design/animation-runtime.md) section 9) |
| The frame that applied a change painted the destination, then jumped back to the start | `FramePipeline::sample_started_animations` paints the first value in that frame |
| Fading in from `transparent` passed through dark grey | colour interpolation is premultiplied (CSS Color 4) |
| Text in a single-line `TextInput` sat at the top of the field | `paint_text.cpp` centres the line in the content box ([text-editing-v1.md](../specs/text-editing-v1.md)) |

## 6. Known limits

- Animation, measured on the segment: the selected segment's fill matches Chrome to within 1 of 255 levels at
  0, 50, 100, 150 and 400 ms. One difference remains: `box-shadow` is not transitioned (it snaps to its end
  value, where Chrome fades it). The segment's base fill is `transparent` again, so the fade-in is compared
  from a transparent base; the shadow behind it is clipped to outside the border box (issue #461).

- `background-image: linear-gradient(...)` from a stylesheet paints nothing: `PaintData::gradient` exists but no
  style property feeds it ([stylesheet-v1.md](../specs/stylesheet-v1.md) lists gradients as a V1 non-goal). The
  dashboard therefore uses a solid hero; a gradient there is the clearest visible gap against Chrome.
- A border with different widths or colours per side is not one ring and keeps the square edge strips
  on a rounded box.
- The font has one weight, so bold is not compared. The form runs in the light theme and the dashboard in the dark one, both at
  device scale 1; high contrast, RTL and scale 2 need their own runs.
- The engine's element boxes are not exported, so layout is compared through the overlay, not as
  numbers.
