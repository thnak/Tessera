# Layout: Yoga Flexbox and a small Grid subset

Status: design for Big Step B2. `src/ui/layout/` is a placeholder today. ADR-005 requires the
Yoga adapter and the documented rounding rule below.

## 1. Goals

- CSS-like Flexbox through **Yoga**; no hand-written Flexbox.
- A small CSS Grid subset on top of Yoga or a vendored engine (taffy) behind a C ABI.
- Intrinsic text measurement via a callback into the text stack.
- Fractional layout internally; deterministic integer-pixel paint alignment.
- Incremental layout from dirty flags; no full-tree relayout for local changes.

Non-goals in V1: subgrid, masonry, dense auto-placement, full CSS tables, browser quirks.

## 2. Pipeline

```text
ComputedStyle (layout longhands)
  -> LayoutNode tree (mirrors scene nodes)
  -> Yoga node mapping
  -> measure callbacks for text/images
  -> calculateLayout(available width/height, direction)
  -> LayoutResult (x, y, width, height) per node
  -> paint-time pixel snap
  -> scene bounds updated -> paint dirty
```

The scene graph owns node identity; the layout tree is a parallel structure with the same
shape. It is rebuilt/updated on structural changes and reused otherwise.

## 3. Style mapping

### Flexbox

| CSS | Yoga |
|---|---|
| `display: flex` | `YGNodeStyleSetDisplay(YGDisplayFlex)` |
| `flex-direction` | `YGFlexDirection*` |
| `flex-wrap` | `YGWrap*` |
| `justify-content` | `YGJustify*` |
| `align-items` / `align-self` / `align-content` | `YGAlign*` |
| `flex-grow` / `flex-shrink` / `flex-basis` | `YGFlexGrow`/`Shrink`/`Basis` |
| `gap`, `row-gap`, `column-gap` | `YGNodeStyleSetGap` |
| `padding`, `margin`, `border` | Yoga edge setters |
| `width`/`height`/`min`/`max` | Yoga dimension setters |
| `position: relative/absolute` | `YGPositionTypeRelative/Absolute` |
| `inset` | Yoga position edges |
| `aspect-ratio` | `YGNodeStyleSetAspectRatio` |
| `overflow` | `YGOverflow*` |

Percentages are passed symbolically to Yoga; `auto` maps to `YGValueAuto`. `box-sizing` is
normalized before mapping: V1 follows the CSS `content-box` default, with `border-box`
supported explicitly. Yoga sees consistent dimensions in either mode.

### Grid subset (V1)

Supported:

- fixed and `repeat()` columns/rows
- `gap`
- item `grid-column`/`grid-row` span
- basic `justify-items`/`align-items`

Not supported: subgrid, masonry, dense auto-placement, named lines/areas beyond a small
subset. If taffy is used, its C ABI is hidden behind `GridEngine`; UI code never includes it.

### Positioning

- `static`/`relative`: normal flow + offset.
- `absolute`: out of flow, positioned against the nearest positioned ancestor.
- `fixed`: viewport-relative (window/scroll container policy documented).
- `sticky`: implemented in the scroll/layout pass, not Yoga; scroll-container semantics are in
  [`scroll-and-gestures-v1.md`](../specs/scroll-and-gestures-v1.md).

## 4. Measurement

```cpp
struct MeasureConstraints {
    float available_width;
    float available_height;
    MeasureMode width_mode;   // undefined, exactly, at_most
    MeasureMode height_mode;
};

struct MeasureResult {
    float width;
    float height;
    float baseline;           // optional
};

using MeasureText = std::function<MeasureResult(
    std::string_view text, const TextStyle&, const MeasureConstraints&)>;
```

Rules:

- The text stack owns shaping/line breaking; layout never rasterizes glyphs.
- Measurement is cached by `(text, style, constraints)`; invalidated by text/font/style change.
- Images use intrinsic size/aspect ratio; missing assets use a deterministic placeholder.
- Yoga baseline alignment uses the text baseline, not the box bottom.

## 5. Pixel alignment

Layout keeps fractional geometry. At paint time, snap **edges**, not sizes, to avoid gaps:

```text
scale = max(dpi_scale, 1.0)
left   = round_half_up(x * scale) / scale
right  = round_half_up((x + width) * scale) / scale
top    = round_half_up(y * scale) / scale
bottom = round_half_up((y + height) * scale) / scale
paint_width  = right - left
paint_height = bottom - top
```

`round_half_up(v) = floor(v + 0.5)` for non-negative coordinates; negative coordinates use
`ceil(v - 0.5)` to keep symmetry. Golden mode uses `scale = 1.0` and the same rule.

The rounding rule is a compatibility contract: changing it requires a golden update and a
documented note.

## 6. Direction and writing mode

- `direction: ltr/rtl` is passed to Yoga (`YGDirectionLTR/RTL`).
- RTL flips start/end edges, not explicit left/right.
- Text bidi/shaping is the text stack's job; layout receives measured runs.
- Vertical writing modes are parsed but layout is LTR/RTL-first in V1.

## 7. Dirty and incremental layout

- Structural change (child add/remove/reorder) -> mark parent layout dirty.
- Layout-affecting style change (size, flex, padding, margin, position, gap, font, text) ->
  mark node layout dirty.
- Window/DPI/safe-area/orientation change -> mark root layout dirty.
- Text/font change -> invalidate measurement cache and mark layout dirty.
- Layout result change -> update scene bounds and mark paint/render dirty.
- Subtree-affecting changes (`display:none`, visibility, transform/clip) follow
  [`dirty-rendering.md`](dirty-rendering.md).

Yoga nodes are reused; avoid rebuilding the Yoga tree every frame. Rebuild only on structural
changes, and measure only dirty subtrees where safe.

## 8. Performance

- Target: 10k nodes with a shallow tree in a single-digit millisecond budget on desktop.
- Avoid allocations in steady state; use node arenas/pools.
- Cache measurements and computed layout styles.
- No string comparisons in the hot path; use property ids/enums.
- Profile layout separately from style/paint; record metrics in the diagnostics overlay.

## 9. Tests

- Box corpus: width/height/min/max, padding/margin/border, percentages, aspect ratio.
- Flex corpus: direction, wrap, grow/shrink/basis, justify/align, gap, absolute children.
- Grid corpus: fixed/repeat, span, gap, alignment.
- Text measure: intrinsic size, wrapping, baseline, ellipsis.
- RTL: start/end mapping, mirrored layout.
- Rounding: edges at 1.0/1.25/1.5/2.0 scale; no gaps/overlaps.
- Incremental: dirty-only layout equals full relayout for the same inputs.
- Golden: fixed viewport/DPI/fonts; byte-identical layout dumps.

## 10. Open questions

- Grid engine: build a subset on Yoga vs vendor taffy behind a C ABI.
- `box-sizing` default and browser-divergence policy.
- Sticky positioning and scroll-container semantics.
- Container queries and their interaction with layout invalidation.
- Baseline alignment for mixed fonts/emoji.
