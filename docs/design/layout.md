# Layout: Yoga Flexbox and a small Grid subset

Status: implementation started for Big Step B2 (runtime slice; see
[AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). The Flexbox adapter is
`include/tessera/ui/layout/layout_engine.hpp` + `src/ui/layout/layout_engine.cpp`, backed by vendored
Yoga v3.2.2. It maps `ComputedStyle` longhands to a parallel `LayoutInput` tree and returns fractional
absolute boxes in DFS order. Non-empty leaf `text` is measured through `ui/text/text_measure.hpp` and
a Yoga measure callback, backed by a bounded `TextMeasureCache` (capacity configurable per engine);
the default `StubTextMeasurer` is deterministic until the real text stack lands. `apply_layout` maps the absolute boxes back onto scene `Node`s as parent-relative bounds,
leaving unchanged boxes clean and marking changed ones paint/render dirty. `LayoutScheduler`
(`ui/layout/layout_scheduler.hpp`) caches the last result and reuses it only while the
layout-input signature (`ui/layout/layout_signature.hpp`, section 7.1) is unchanged, so clean
frames and spurious layout dirt (paint-only style changes) reuse boxes while any real input change
reflows. Independent
subtrees (overlays/popups) can be relaid out in isolation with `LayoutEngine::layout_subtree` +
`LayoutScheduler::relayout_subtree`, which patches the cached boxes while preserving the subtree
root's parent offset; structural removals inside the subtree still require `invalidate()` or a
full update. Yoga has no general partial-layout API, so parent reflow still recomputes the tree.
`add_scene_damage` consumes a `DirtyPass` result, filters `Paint`/`Render` nodes,
snaps their world bounds with `pixel_snap`, and feeds the damage accumulator. `DamageTracker` retains
the previous world bounds per node (bounded LRU), damages both old and new rects when a node moves,
and prunes ids no longer present in the scene.
[ADR-005](../../decisions/ADR-005-yoga-layout-adapter.md) (Yoga layout adapter) requires the adapter
and the documented rounding rule below. Terms (`ComputedStyle`, Yoga, golden, scene graph) are in
[`glossary.md`](../reference/glossary.md). Style inputs come from the stylesheet engine
([`stylesheet-v1.md`](../specs/stylesheet-v1.md)).

## 1. Goals

- CSS-like Flexbox through **Yoga**; no hand-written Flexbox.
- A small CSS Grid subset on top of Yoga or a vendored engine (taffy, a Rust layout library listed as a candidate in
  [`dependencies.md`](../specs/dependencies.md)) behind a C ABI.
- Intrinsic text measurement via a callback into the text stack.
- Fractional layout internally; deterministic integer-pixel paint alignment.
- Incremental layout from dirty flags; no full-tree relayout for local changes.

Non-goals in V1 (first release; scope rule in [AGENTS.md Non-negotiables](../../AGENTS.md#non-negotiables) item 8): subgrid, masonry, dense auto-placement, full CSS tables, browser quirks.

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
normalized before mapping: the default value is `content-box` (the CSS default, set in
[`stylesheet-v1.md`](../specs/stylesheet-v1.md) section 11); `content-box` and `border-box` are
both supported. Yoga sees consistent dimensions in either mode.

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
- `fixed`: viewport-relative (the window/scroll-container policy is not specified in this doc).
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

Layout keeps fractional geometry. At paint time, snap **edges**, not sizes, to avoid gaps. The
contract is implemented by `include/tessera/ui/layout/pixel_snap.hpp` (`round_half_up`,
`snap_scalar`, `snap_rect`):

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
`ceil(v - 0.5)` to keep symmetry. Golden mode (deterministic reference-image tests, [`golden-tests-v1.md`](../specs/golden-tests-v1.md)) uses `scale = 1.0` and the same rule.

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
changes, and measure only dirty subtrees (a subtree whose text/font/style did not change keeps its cached measurement).

### 7.1 Layout-input signature

`layout_signature(root, viewport)` (`ui/layout/layout_signature.hpp`) folds the complete layout
input into one deterministic FNV-1a 64-bit value in a single allocation-free pass over the
`LayoutInput` tree. `LayoutScheduler::update` computes it every call and reuses the cached
`LayoutResult` when it matches the value stored with that result, then counts a `cache_hits()`
entry and does not call Yoga. A mismatch reflows, stores the new signature and counts
`layout_runs()`. `needs_layout` is only a hint: a caller may report layout dirt conservatively, but
a reported dirt whose inputs are unchanged is spurious and is safe to drop.

Included in the signature:

- **Tree shape**: node id, child count and child order, recursively. Node ids are included because
  a box belongs to an id, so a re-keyed identical geometry is still a different result.
- **Layout longhands**, exactly the computed properties `apply_style`
  (`src/ui/layout/layout_style.cpp`) reads: `display`, `flex-direction`, `flex-wrap`,
  `justify-content`, `align-items`, `align-content`, `align-self`, `position`, `overflow-x`/`-y`,
  `direction`, `box-sizing`, `width`/`height`/`min-*`/`max-*`, `flex-basis`/`-grow`/`-shrink`,
  `aspect-ratio`, `margin-*`, `padding-*`, `border-*-width`/`border-*-style`, `top`/`right`/
  `bottom`/`left`, `row-gap`/`column-gap`, and `field-sizing` (it decides whether an empty leaf is
  a measured node).
- **Text measurement longhands**, exactly what `text_style_from` reads: `font-size`,
  `line-height`, `letter-spacing`, `font-weight`, `font-style`, `white-space`.
- **Measured text**: the node's `text` bytes. A text change can change a measured leaf even when
  the surrounding box has fixed dimensions, so it always invalidates; the guard does not try to
  prove text changes geometry-neutral.
- **Viewport**: `width`, `height`, `dpi_scale` (each as a 32-bit bit pattern).

Deliberately excluded: paint-only and interaction longhands, which cannot move a box — all colors,
backgrounds, borders' `*-color`/`border-radius`, `box-shadow`, `opacity`, `visibility`,
`transform`, `z-index`, `text-overflow` (documented paint-only in `text_measure.hpp`),
`font-family` (the measure seam receives `text::TextStyle`, which carries no family), and any
custom property. `ComputedStyle` is a sorted map, so declaration insertion order does not affect
the signature, and only present layout properties are folded in: removing a property whose value
equalled Yoga's default still triggers a (safe, redundant) reflow.

Guarantee: two inputs with the same signature produce the same boxes, because the signature covers
everything `apply_style` and the measure callback consume, and both are pure functions of those
inputs. A hit is therefore equivalent to a reflow for the same signature; the only gap is the
64-bit hash collision probability. The equivalence test
(`tests/unit/layout_signature_tests.cpp`) asserts the guarded boxes field-for-field against a fresh
engine layout for paint-only, text-only, width, reorder, viewport and invalidate scripts, and
asserts zero reflows for paint-only dirt and exactly one for each real change.

Hit rate in the `AppHost` path: a paint-only inline-style edit
(`Node::set_inline_style("background", ...)`) rebuilt the selector tree and still missed the guard.
The signature was right and the input really did differ. `AppHost::apply_root_size` passed a
temporary string to `set_inline_style` for the host-owned root `width`/`height`, while
`Node::set_inline_style` *borrows* its name/value (the caller keeps the storage alive, as generated
code does with literals). The temporary was freed at the end of the constructor expression, so the
computed root values were read from freed bytes; once a rebuild reused that heap block, the input
became `width: "320px"` -> `width: <freed bytes>` and the guard correctly reported a changed input.
The rule: an inline-style value must outlive the declaration it is stored in. `AppHost` now formats
the root size into host-owned buffers (stable addresses for the host's lifetime, re-formatted in
place on resize), so a rebuild produces a byte-identical input and the paint-only edit is a hit.
The cascade never re-serializes unrelated longhands: a value that reaches `ComputedStyle` is a
copied `std::string`, and the guard hashes only the properties in the include lists above, so a
re-resolved default cannot masquerade as a layout change.

`invalidate()` clears both the result and the signature. A successful `relayout_subtree` also drops
the signature: the patch is a subtree snapshot resolved against the subtree root's own width and
siblings are left untouched, which is an approximation of a full reflow. The next `update`
therefore reflows, bounding the patch to the case it was designed for (an independent
overlay/popup) instead of letting a later hit stand for the approximation.

## 8. Performance

- Target: 10k nodes with a shallow tree in a single-digit millisecond budget on desktop (reference hardware is not specified).
- Avoid allocations in steady state; use node arenas/pools.
- Cache measurements and computed layout styles.
- No string comparisons in the hot path; use property ids/enums.
- Profile layout separately from style/paint; record metrics in the diagnostics overlay ([`diagnostics.md`](diagnostics.md)).

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
- Browser-divergence policy for `box-sizing` (the default is `content-box`; any difference from browsers is not yet listed).
- Sticky positioning and scroll-container semantics (see [`scroll-and-gestures-v1.md`](../specs/scroll-and-gestures-v1.md)).
- Container queries and their interaction with layout invalidation (see [`responsive.md`](responsive.md)).
  Proposed answer, with component size observation and a layout read model:
  [`layout-observation.md`](layout-observation.md) (ADR-041, Proposed).
- Baseline alignment for mixed fonts/emoji.
