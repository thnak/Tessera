# Several background layers (issue #614)

Status: implemented (`include/tessera/ui/style/background_layers.hpp`,
`src/ui/style/background_layers.cpp`, `src/ui/style/property_background.cpp`,
`src/ui/paint/paint_gradient.cpp`). Paint-pass context: [paint-pass.md](paint-pass.md) section 13;
the single-gradient grammar: [`stylesheet-v1.md`](../specs/stylesheet-v1.md) section 11.

## 1. The value

CSS Backgrounds 3 section 2.1 makes `background-image` a comma-separated list of images. V1
implements the list of `linear-gradient()` layers plus `none`:

```css
background-image: linear-gradient(180deg, rgba(239, 68, 68, 0.5), rgba(239, 68, 68, 0.5)),
                  linear-gradient(90deg, #22C55E, #FACC15);
```

- A `url()` layer is **not** in scope: images are not a `background-image` value the engine paints
  yet, so the whole declaration is rejected with one `UnsupportedValue` diagnostic rather than
  silently dropped. A radial/conic layer (#613, #617) is rejected the same way.
- The `background` shorthand puts the colour on the **final layer only**, as in CSS; a colour on any
  other layer is a bad value and rejects the declaration. The colour lands in `background-color`,
  which paints underneath every layer.
- At most `kMaxBackgroundLayers` (8) layers, so the value is fixed-size and parsing stays
  allocation-free.

## 2. The type

`BackgroundLayerList` is a list of `BackgroundLayer`, each with a `BackgroundLayerKind` tag and the
fields that kind uses. The tag is what makes a radial/conic or `url()` layer a new enumerator plus a
new field instead of a rewrite of the parse, the paint order and the interpolation contract.

`layers[0]` is the **topmost** layer and `layers[count - 1]` is painted first, which is the CSS
order: the value reads top to bottom and the paint runs bottom to top.

## 3. Paint order

`paint_linear_gradient` walks the list **backwards** and emits one `PaintSink::fill_linear_gradient`
per gradient layer. Every layer is a full-box fill, so a lower layer is visible only where the
layers above it are translucent. A `none` layer paints nothing but keeps its slot.

The node's effective opacity folds into every layer's stop alphas, not just the first. A
`PaintData::gradient` payload still wins over the stylesheet and is treated as a one-layer list, so
a scene that sets a gradient in code is unchanged.

Both sinks receive one `GradientDraw` per layer, so CPU and GPU agree by construction: the 64-band
emission and the shared encoded-space ramp are the same code on both sides.

## 4. Interpolation

`interpolate_background_layers` blends layer by layer, following CSS Images 4 section 3.1:

- The shorter list is padded at the **bottom** (the end of the list) with `none`, so a layer added
  on top of a shorter list fades in over the layers that already exist instead of shifting them.
- A `none` pairs with the **transparent form** of the other layer: the same stops and angle with
  every alpha at 0. That is what `none` denotes, so a layer fades in or out instead of snapping.
- Two gradients blend their angle along the shorter arc (350deg to 10deg goes through 0, not 180)
  and their stops pairwise. A stop-count mismatch repeats the shorter list's last stop, as CSS does.
- The colour blends premultiplied through `style::interpolate_color`, so a fade through
  `transparent` does not pass through dark grey.

A value outside the grammar returns `nullopt`, so the caller keeps its discrete 50 % switch.

## 5. Tests and baselines

| Test | What it proves |
|---|---|
| `background_layers` | unit: the grammar (including the `url()`/radial/trailing-comma/9-layer rejections), the printed form round-trips, the shorthand's final-layer colour rule, the interpolation (channel blend, bottom padding, `none` fade, shorter arc), and the paint order through a recording sink |
| `paint_gradient_gpu` | real Vulkan readback: the bottom ramp shows through the translucent top layer, and the CPU sink agrees within 2/255 per channel |
| `cpu_wide_golden` `layers` | the committed CPU baseline `tests/golden/baseline/cpu_wide/layers.png`: a two-layer stack, a three-layer stack with a `none` in the middle, and a single opaque layer |

The CPU baseline is generated on the CPU path only, through the test's own write mode:

```bash
./build/<dir>/tests/gpu/cpu_wide_golden_tests --update-goldens   # layers.png
```

It is compared bit-exactly (tolerance 0, zero mismatched pixels), so it is bit-exact on Windows and
Linux: no GPU, lavapipe, wall clock, random id or system font is involved and no text is painted.
Regenerating it must not change a byte.

## 6. Deferrals

- Radial and conic layers (#613, #617): a new `BackgroundLayerKind` enumerator plus its field.
- `url()` layers: the same, once images are a `background-image` value the engine paints.
- Per-layer `background-position`/`-size`/`-repeat`/`-clip`: the shorthand rejects them today.
