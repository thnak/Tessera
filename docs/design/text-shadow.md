# `text-shadow` (issue #616)

Status: implemented (`src/ui/paint/paint_text_shadow.{hpp,cpp}`, `paint_pass.cpp`,
`src/ui/animation/shadow_interpolation.cpp`). Paint-pass context:
[paint-pass.md](paint-pass.md) section 10; the box-shadow precedent this follows:
[gpu/box-shadow-clipping.md](gpu/box-shadow-clipping.md); text runs and ink measurement:
[text.md](text.md) section 6.

## 1. The rule

CSS paints a text shadow **behind the glyphs** of the text it applies to, offset by `<x> <y>`,
optionally blurred by `<blur>`, in the declared colour (CSS Text Decoration 3 section 3). The
grammar is `[<color>? <x> <y> <blur>?]`, comma separated, up to a bounded count. Unlike
`box-shadow` there is **no spread and no `inset`**: a fourth length or an `inset` token is a
grammar error, not an unimplemented V1 form, so the declaration is dropped with a diagnostic.

Three properties of the rule drive the implementation:

- The shadow is a *paint* effect. It must not change layout, and it must not change the glyph ink
  box that the culling and text-fit code reads (section 3).
- It applies to **every run** of the text, including the runs of inline spans, the lines of a
  wrapped paragraph, the ellipsized line and the IME preedit.
- Every shadow of a text block sits behind **every** glyph of that block. Emitting each run's
  shadow immediately before that run would let the next line's shadow darken the previous line's
  glyphs, which is not what a browser paints.

## 2. Where it is emitted

`paint_text_lines` installs a `TextShadowSink` around the sink it was given, and the payload
emission runs through it. The wrapper buffers the `TextDraw`s the payload produces and, at
`flush()`, emits every shadow of every buffered run back to front and then the runs themselves.
That is the whole reason for a sink wrapper rather than a call at each of the seven `TextDraw`
sites: the payload has seven emission paths (single-line fast path, wrapped lines, span slices,
ellipsized line, preedit run, decoration underline, caret), and a shadow has to cover all of them
without any of them knowing about it. `LcdTextSink` uses the same seam for the same reason.

Ordering inside the wrapper:

```text
for each shadow, last declared first:      # CSS paints the first shadow on top
    for each buffered run:
        emit the shadow copy
for each buffered run:
    emit the run
```

The wrapper is installed *outside* `LcdTextSink`, so a shadow copy is never marked
`TextDraw::lcd`. A blurred impression of the glyphs must not composite per-channel coverage: LCD
fringes on a shadow would be a colour artefact no browser paints.

## 3. Layout and the ink box are untouched

The shadow is emitted as ordinary `TextDraw` runs offset from the real ones. Nothing about the
node's box, the line boxes, the baselines, the measure cache or the text-fit fact changes, so:

- layout produces the same boxes with and without a shadow;
- the glyph run the sink receives keeps the unshadowed rect, which is the ink box the culling and
  fit code reads;
- `Node::text_fits_box` stays valid, so a shadowed text node is still cullable.

What *does* change is the node's **paint reach**: a shadow paints outside the text box. The reach
is a property of the style alone (offsets and blur, not the glyphs), so `sync_scene_style` folds
`text_shadow_outsets()` into `Node::set_paint_outsets` alongside the box-shadow reach. That is what
keeps the cull test and the damage region correct without ever growing the ink box. The outsets are
`blur ± offset` per side, the same formula `box-shadow` uses.

## 4. The blur approximation

A real Gaussian blur of glyph coverage needs an offscreen layer, which the batched renderer does
not have (paint-pass.md section 6). The shadow is therefore a **bounded stack of offset copies** of
the glyph run, the text analogue of `box-shadow`'s concentric fills:

- `sigma = blur / 2`, the same convention `box-shadow` uses, so the two effects fade alike.
- The stack has `clamp(ceil(blur / 2), 2, 4)` rings. Ring *j* sits at radius
  `blur * (rings - j) / rings` and carries `kTextShadowRingSamples = 8` copies at 45° steps.
- Each ring's alpha is chosen so the cumulative "over" composite at that radius equals the Gaussian
  coverage `0.5 * (1 - erf(r / (sigma * sqrt 2)))` there - the same cumulative-alpha recurrence
  `paint_shadow.cpp` uses for its concentric fills.
- A final centred copy carries the full colour, so the glyph interior is solid rather than a
  translucent stack.

The cost bound is therefore `4 rings * 8 samples + 1 = 33` extra draws per run per blurred shadow,
and `kMaxTextShadows = 4` caps the list. `PaintContext::reduce_effects` (adaptive quality,
performance.md section 6.1) caps a blurred shadow at one ring, which keeps the reach and the colour
and coarsens the edge.

The 45° sample table is a constant rather than `sin`/`cos` calls: the paint path stays free of
trigonometry and the emitted geometry is exactly reproducible, which is what a golden compares.

A sharp shadow (`blur: 0`) is one copy, so the common case costs one extra draw.

## 5. Interpolation

`text-shadow` animates through the same layer walk as `box-shadow`
(`interpolate_text_shadow`, `shadow_interpolation.cpp`): offsets and blur interpolate as lengths,
the colour as a premultiplied colour, and the shorter list is padded with transparent zero shadows
so `none` fades a shadow in or out instead of snapping at 50 %. The two fields the grammar lacks
are pinned: `spread` stays 0 and `inset` is rejected, so a value carrying either is not a
`text-shadow` and the caller keeps its discrete switch. The formatter omits the spread, so the
emitted value is always parseable by the paint pass.

## 6. Tests and evidence

| Test | What it proves |
|---|---|
| `paint_text_shadow` | unit: the grammar (colour in either position, `currentColor`, lists, and the invalid forms - one length, `inset`, a fourth length, a negative blur, one bad layer); every shadow precedes every glyph of a wrapped block and matches its line's offset; span slices get their own copies; the blur stack is bounded and fades outward; `reduce_effects` emits fewer copies; opacity folds into the shadow alpha; a shadow copy is never LCD while the glyph run stays LCD; the node box and the glyph run rect are byte-identical with and without a shadow; the outsets carry the reach; a node with no text payload paints no shadow; the counter counts copies |
| `text_shadow_interpolation` | unit: offsets/blur/colour interpolate, `none` fades in, lists pad, spread and `inset` do not interpolate, and the keyframe sampler produces a mid-transition value |
| `paint_text`, `paint_text_span`, `paint_pass` | the existing text and box-paint suites stay green, so the wrapper is transparent when there is no shadow |

No new image golden: the CPU canvas has no text rasterizer (paint-pass.md section 10), so a
text-shadow golden would have to come from the GPU path, and the existing lavapipe text goldens
come from the direct `TextRenderer` path rather than the paint pass. The behaviour is pinned by the
deterministic recording-sink assertions above, which is the same choice `paint_text` already makes.

## 7. Limits

- The blur is an offset-copy stack, not a true Gaussian: the edge is a ring of hard copies rather
  than a continuous falloff. At the ring counts above the difference is below a pixel at typical
  blur radii, and the reach is exact.
- No spread and no `inset` (CSS has neither for `text-shadow`).
- Lengths are px or a bare `0`; percentages and `em` are not resolved (the same limit the paint
  pass documents for every other length).
- The shadow is grayscale-composited even when the run is LCD-eligible (section 2).
