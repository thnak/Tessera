# Scene paint pass (box visuals)

Status: first slice implemented in `src/ui/paint/` (library `tessera_paint`, namespace
`tessera::ui::paint`). It is the missing link between the scene graph and
[`Batched2DRenderer`](../../include/tessera/rendering/gpu/batched2d_renderer.hpp): a deterministic,
GPU-free pass that turns a scene subtree plus computed styles into background and border quads.

## 1. Where it sits

```text
stylesheet (compiled) --> ComputedStyle per node
scene graph (bounds, visibility) --> paint_scene --> PaintSink --> Batched2DRenderer
```

The pass reads, never mutates, the scene. It does not parse CSS and does not know about Vue/React,
layout, or the redraw plan. The caller owns the compiled stylesheet and passes a `StyleProvider`;
[`SceneStyleProvider`](../../include/tessera/ui/paint/paint_pass.hpp) wraps a
`style::CompiledStyleSheet` plus a `style::SceneStyleTree`, and tests supply a stub provider.

This is the paint stage of the stage order described in [`ui-model.md`](ui-model.md) (style -> layout
-> paint -> render). Damage/redraw (see [`dirty-rendering.md`](dirty-rendering.md)) reaches it through
[`FramePipeline::paint_frame`](#8-frame-integration): the full-frame overload ignores the plan and the
damage-clipped overload clips the sink to the `RedrawPlan` scissor regions.

## 2. Why a sink seam

[`PaintSink`](../../include/tessera/ui/paint/paint_sink.hpp) has two operations, both axis-aligned:

| Operation | Meaning |
|---|---|
| `fill_rect(rect, color)` | solid rectangle |
| `fill_rounded_rect(rect, color, radius)` | solid rounded rectangle (SDF path in the renderer) |

A recording sink makes every rect, color, radius, and emission order assertable in unit tests with
no device, driver, or window.
[`Batched2DSink`](../../include/tessera/ui/paint/batched2d_sink.hpp) is the only adapter and forwards
to `Batched2DRenderer::{push_rect,push_rounded_rect}`. Its header forward-declares the renderer, so
the paint API never pulls in a backend header.

## 3. Coordinates

`Node::bounds()` is parent-relative ([`node.hpp`](../../include/tessera/ui/scene/node.hpp)) and
`Node::world_bounds()` composes the box origins of the ancestor chain and the node transforms into
absolute coordinates, so the pass paints that rect directly:

```text
paint_rect(node) = world_bounds(node)
```

- The scene contract is the one `apply_layout` produces: layout stores parent-relative bounds on
  each node and leaves transforms identity; `world_bounds()` resolves the chain.
- `world_bounds()` is the axis-aligned box of the transformed box-local rect; rotation/scale are
  approximated by that AABB.
- Clipping (overflow and scroll containers) is a clip push around the children; the node's own
  background/border still paint over its border box. The clip rect is the padding box: the border
  box inset by the painted border widths (identical to the border box when there is no border).

### 3.1 Scroll containers

Scroll state is additive scene state ([`scroll.hpp`](../../include/tessera/ui/scene/scroll.hpp)):
`Node::world_transform()` places every child in its parent's content space, so a container's children
paint at `world_bounds() - scroll` while the container's own box is unchanged. A node with
`scrollable_x() || scrollable_y()` is treated as a clipping parent: `paint_scene` pushes the same
rounded padding-box clip used for `overflow` around its children and pops it after them
(`PaintStats::clips`); a border-inset that consumes the box skips the clip rather than pushing a
degenerate one. Layout is never mutated by scrolling, and damage for a scroll is the container's
border box (the children are clipped inside it). The scrollport for clamping is still the border box
until layout supplies padding-box scrollport insets; scrollbars and physics are later slices.

## 4. Property mapping

| Property | Use |
|---|---|
| `background-color` | fills the node's paint rect |
| `border-{top,right,bottom,left}-width` | edge thickness in px |
| `border-{top,right,bottom,left}-style` | `none`/`hidden` (or missing) skips the edge; any other keyword paints solid |
| `border-{top,right,bottom,left}-color` | edge color; `currentColor` uses `color` |
| `border-*-radius` | when all four are equal and > 0, the background is one rounded rect |
| `color` | inherited `currentColor` base; invalid/missing falls back to black |
| `opacity` | effective opacity: own value x ancestor product, multiplies every emitted color (§14.1) |

Parsing rules:

- Lengths accept `px` and bare numbers; percentages, `em`, and other units are unsupported and the
  property is ignored. Missing lengths use the property's initial value (0 for widths/radii).
- Colors use [`style::parse_color`](../../src/ui/style/color.cpp), so named/hex/rgb/rgba/hsl work.
  `transparent` and any zero-alpha color are skipped, as are unparseable values.
- `opacity` is not inherited, but its effect composites the node's whole subtree: see
  [§14.1](#141-effective-opacity-issue-227).

## 5. Paint order and edges

Per node, in this order:

1. background (rounded when the shared radius is > 0, clamped to half the shorter side);
2. top edge, right edge, bottom edge, left edge;
3. children, in `Node::children()` order (paint order, last child on top).

Edge rects are inside the node box, so borders paint over the background. Opposite widths are
clamped (`top + bottom <= height`, `left + right <= width`) and the side strips exclude the corners
already covered by the top/bottom strips:

```text
top    { x, y,                    w,              top }
right  { x + w - right, y + top,  right,          h - top - bottom }
bottom { x, y + h - bottom,       w,              bottom }
left   { x, y + top,              left,           h - top - bottom }
```

With a rounded background the border strips keep square corners: corner radii are a background
property in this slice, not a border ring.

## 6. Deliberate limits

- No text, glyphs, or inline content in the box pass; text runs are painted from
  optional `ui::PaintData` payloads (§10).
- Overflow clipping is rounded-rect only ([§9](#9-rounded-overflow-clips)); no `clip-path`, masks,
  filters, gradients, shadows, or images.
- Scroll containers clip and offset children ([§3.1](#31-scroll-containers)); scroll physics,
  inertia/spring-back, scrollbars, snap/sticky, and nested scroll chaining are later slices.
- No damage/redraw plan inside `paint_scene`: every call repaints the visible subtree it is given.
  The frame pipeline clips around it with a sink decorator ([§8.1](#81-damage-clipped-painting)).
- No isolated-layer group opacity; effective opacity is multiplied per primitive (§14.1), so
  overlapping siblings composite separately and a group cannot fade as one layer.
- No percentage/`em`/`rem`/viewport units and no non-pixel radii.
- Border styles other than `none`/`hidden` are painted solid (no dashed/dotted patterns).
- Transform composition beyond the `world_bounds()` AABB is out of scope.
- `SceneStyleProvider` computes a `ComputedStyle` (a `std::map`) per visible node; retained style
  caching and allocation-free hot paths are a later slice.

## 7. Testing

[`tests/unit/paint_pass_tests.cpp`](../../tests/unit/paint_pass_tests.cpp) registers `paint_pass`
and covers: absolute placement through parent offsets, parent-before-child order, invisible subtree
skipping, transparent/zero-alpha/unparseable colors, the four border edges with distinct colors,
`none`/`hidden` sides, uniform vs mixed radii, radius clamping, opacity and `currentColor`, an empty
scene, the `SceneStyleProvider` path, and a null-backend integration that asserts two draws and never
submits; effective opacity is covered by `paint_opacity` (§14.1).

Determinism: no clock, fonts, network, or GPU; colors and rects are compared exactly (alpha within a
small epsilon), and the recorder captures emission order.

Overflow clips (§9) are covered in `paint_pass`: push/pop order, per-corner radius clamping,
rectangular clips, `overflow: visible`, nesting, and the bordered padding-box rect. Scroll containers
are covered in [`tests/unit/scroll_tests.cpp`](../../tests/unit/scroll_tests.cpp) (`scroll_core`:
child rects shifted by `-scroll`, padding-box clip, balanced pop, deterministic repeat) and
`tests/unit/paint_clip_tests.cpp` (bordered padding-box clip, scrolled child position).

## 8. Frame integration

[`FramePipeline`](../../include/tessera/ui/frame_pipeline.hpp) owns the per-frame order and exposes
the paint stage through two bounded calls:

```cpp
// Full-frame paint: every visible box.
FramePaint FramePipeline::paint_frame(const Node& root, paint::StyleProvider& styles,
                                      paint::PaintSink& sink, bool force = false) const;

// Damage-clipped paint: only the parts of the scene inside the plan's regions.
FramePaint FramePipeline::paint_frame(const Node& root, paint::StyleProvider& styles,
                                      paint::PaintSink& sink, const rendering::RedrawPlan& plan,
                                      bool force = false) const;
```

`FramePaint` carries the `paint::PaintStats` and a `painted` flag, so "frame was not due" is
distinguishable from "painted an empty subtree" without inspecting the sink.

Frame order per iteration:

```text
tick(root, dt)              # animation samples, dirty collection, needs_style/needs_layout
  (caller: style re-resolve / layout if the tick asked for it)
finish_frame(root)          # damage + normalized redraw plan -> should_render
paint_frame(root, styles, sink)                     # full-frame mode
paint_frame(root, styles, sink, render.redraw_plan) # damage-clipped mode
  (caller: submit the command list / present)
```

Contract:

- **Only due frames paint.** `finish_frame()` records `FrameRender::should_render` as the pipeline's
  frame decision; both overloads are a no-op (`painted == false`, no stats, no sink calls) when the
  frame is not due. `tick()` clears the decision, so a new frame cannot paint until its own
  `finish_frame()`. The explicit `force` flag paints anyway (for example after a resize or a backend
  reset); it does not change the frame decision.
- **Read-only scene.** `paint_frame()` takes `const Node&`; it does not mark or clear dirty flags,
  does not queue damage, and does not touch `DamageTracker`/`DamageAccumulator`. Style and layout
  decisions (`FrameTick::needs_style`/`needs_layout`) keep their existing semantics.
- **Caller owns the inputs.** The stylesheet/style provider and the sink are supplied by the caller,
  exactly as for `paint_scene`; the pipeline adds no backend, GPU, or device dependency.
  `paint::SceneStyleProvider` over a compiled stylesheet and a `SceneStyleTree` is the default
  provider; tests use a recording sink and a stub provider.
- **Stats are pre-clip.** `FramePaint::stats` are the counters `paint_scene` produced while feeding
  the sink (nodes visited, background/border candidates). A command the clip decorator drops still
  increments its counter; the sink is the source of truth for emitted geometry.
- **Repeatable.** Painting twice in one due frame is allowed and deterministic: the same commands
  and the same `PaintStats` are produced by either overload.

### 8.1 Damage-clipped painting

The clipped overload wraps the caller's sink in an internal decorator that mirrors the damage
semantics of `Batched2DRenderer::begin_damaged` at the paint level, without changing the renderer.
Each paint command is tested against every `RedrawPlan::scissors` region, in region order:

| Command vs region | Emitted |
|---|---|
| fully inside one region | the command unchanged |
| partially inside | the intersection rectangle, same color (and radius inputs) |
| outside every region | nothing |
| `plan.full_redraw == true` | the unclipped command (the scissor list is ignored) |
| empty partial plan | nothing, `painted == false`, zero stats, even with `force` |
| dirty-pass overflow frame | `plan.full_redraw == true` (the damage pass forces it, dirty-rendering §6.6), so the unclipped row applies |

A dirty-pass overflow (more dirty nodes than the `DirtyPass` cap) marks the frame as a full redraw
before painting; the clipped overload therefore receives `plan.full_redraw == true` and emits the
unclipped command stream, exactly like a full-redraw frame. Paint semantics are unchanged: the
overflow rule lives in the damage/plan layer ([`dirty-rendering.md`](dirty-rendering.md) §6.6).

Region semantics:

- A `rhi::Scissor` is the half-open integer rect `[x, x + width) x [y, y + height)`; it is converted
  to a float rect before the intersection, so a command edge on a region edge is inside.
- Paint order is preserved: commands keep their source order and, within one command, regions keep
  the plan order; for disjoint regions the output is independent of the region order.
- `make_redraw_plan` regions are pairwise disjoint (`DamageAccumulator::add` AABB-merges touching
  rectangles, and `begin_damaged` requires disjoint regions). A caller-supplied plan with overlapping
  regions emits the overlapped pixels once per covering region: deterministic, not a renderer plan.
- A full-redraw plan is byte-identical to the full-frame overload: the decorator is bypassed, so
  commands outside the target or outside `plan.scissors` are still emitted.
- Clipping is geometric: every emitted command covers only pixels inside a region, so a caller may
  feed this sink directly; passing the same plan to `begin_damaged` only re-clips (harmless).
- Rounded rects: a contained one keeps its radius; a partially covered one becomes the plain
  intersection rectangle (the API cannot express a clipped SDF shape); an outside one is dropped.

### 8.2 Old and new bounds

`DamageTracker::add_scene_damage` (task-3's frame path) records each merged Paint/Render dirty node's
previous and current snapped bounds, so `finish_frame` emits up to two regions per moved node.
Clipping needs no special case: in the old region the node no longer overlaps, so the repainted
background/parent visuals erase the previous pixels, while the new region repaints the background and
then the node at its current position. The two regions stay separate until the damage accumulator
merges them (when the old and new boxes touch), and each is clipped independently.

### 8.3 Limits of the clipped path

The clip is an axis-aligned geometric intersection, not a rasterization scissor: text runs, clip
paths, and non-axis-aligned geometry are later slices, exactly as for `paint_scene` itself.
Rounded-rect corners at a scissor cut are approximated as described above, and the decorator adds
one command per region a source command intersects (a full-screen background over eight damage
regions emits eight clipped rects).

The frame-pipeline tests in
[`tests/unit/frame_pipeline_tests.cpp`](../../tests/unit/frame_pipeline_tests.cpp) cover: no paint
before a frame decision, no paint on an idle frame, an animation-dirty frame painting once with the
expected background/border rects in paint order, stats (`nodes`/`backgrounds`/`borders`) matching
the painted scene, a scene snapshot (bounds, transform, visibility, dirty flags, child counts)
unchanged across a paint, repeated paints producing identical commands, forced painting of an idle
frame, painting neither queueing damage nor changing the next frame decision, and - for the clipped
overload - a partial plan that clips away outside geometry and leaves contained commands unchanged,
a full-redraw plan that reproduces the full paint, an empty plan (and empty plan plus force) that
paints nothing, forced clipped painting on an idle frame, and a moved node whose old and new
DamageTracker regions repaint the background and the node respectively.

## 9. Rounded overflow clips

`paint_scene` clips a parent's children when its computed `overflow-x`/`overflow-y` is anything but
`visible` (`hidden`, `clip`, `scroll`, and `auto` all clip: the paint pass itself only clips;
scrolling is scene state applied by the world transform, see [§3.1](#31-scroll-containers)). The clip
is
emitted around the children, after the parent's own background/borders, through
`PaintSink::push_clip`/`pop_clip`:

- `RoundedClip { rect, top_left, top_right, bottom_right, bottom_left }`: `rect` is the parent's
  padding box in absolute coordinates (the border box inset by the painted border widths) and each
  radius is already clamped to `min(width, height) / 2`. All-zero radii mean a rectangular clip (the
  renderer keeps the scissor fast path); any non-zero radius switches that draw batch to the
  analytic fragment test.
- The clip is the padding box, the CSS child clip region; the outer radii are reused on the inset
  rect (inner-radius adjustment is not modelled). `clip-path` and masks are out of scope.
- `PaintSink` keeps default no-op clip hooks so existing recording sinks are unaffected;
  `Batched2DSink` forwards to `Batched2DRenderer::push_clip`/`pop_clip`. The damage-clipped paint
  decorator in the frame pipeline forwards clip pushes/pops unchanged, so clipping and damage
  clipping compose.
- Style seam: this needed no stylesheet change. `StyleProvider` already returns the full
  `ComputedStyle`, which carries `overflow-x`/`overflow-y` and the four `border-*-radius` longhands.
- Clip math lives in `rendering::gpu::clip` (`clip.hpp`). Nesting rule: the intersection rect, and
  each corner keeps the largest radius of a clip whose two edges coincide with that intersection
  corner; corners created by a straight cut become square. The rule is exact for containment and
  conservative elsewhere - it may sharpen a cut corner but never paints outside any active clip.
  Depth is bounded at 8; the shader receives at most 3 folded clips and deeper clips are folded
  into the innermost slot with the same intersection rule.
- No clip-edge AA: the mask is a hard pixel-center test (see [`antialiasing.md`](antialiasing.md)),
  so rounded clip corners and edges alias at 1x until MSAA.
- Tests: `paint_pass` (emission order/rect/radii/rectangular/nested), `clip_math` (pure clamping,
  nesting, containment, degenerate radii), `batched2d` (null-backend batch-split rule), and
  `rounded_clip` (real-Vulkan readback: pixels outside a rounded parent corner stay background).
- Goldens: **no re-baseline and no new image golden in this slice.** The CPU/lavapipe golden scenes
  contain no overflow-clipping rounded container (the POC dashboard is built with direct renderer
  calls and scissor clips; the CPU canvas is rectangle-only), so existing golden bytes are
  unchanged (`batched2d_cpu_golden` stays byte-exact). A dedicated lavapipe rounded-clip golden is
  deferred until such a scene enters the corpus and the baseline can be produced on the software
  path (never from a real GPU).

## 10. Text paint rules (D9)

Scene nodes may carry an optional `ui::PaintData` payload
([`paint_data.hpp`](../../include/tessera/ui/scene/paint_data.hpp)): `PaintKind::Text`, UTF-8 `text`
(at most `PaintData::kMaxTextBytes = 4096` bytes; longer input is truncated at a UTF-8 code-point
boundary), and an optional `PaintTextStyle` override. Layout and style never read it;
`Node::set_paint_data` marks `Dirty::Paint|Render` only and is a no-op when the payload is unchanged.
The app/compiler owns text content; visual properties still come from computed style.

Emission: after the node's background and borders, inside the node's own overflow/scroll clip and
the ancestors' clips, before the children (paint order). `PaintSink::draw_text(TextDraw)` is additive
and defaults to a no-op (same pattern as `push_clip`), so recording sinks compile unchanged.
`TextDraw` carries the absolute border-box rect, a borrowed UTF-8 view, resolved font size/weight/
color, and the node opacity folded into alpha; empty text emits nothing.

Style precedence: PaintData override > computed style (`font-size`, `font-weight`, `color`) >
backend default (font size 16, weight 400, black). Missing/invalid computed values fall back to the
defaults; a non-positive effective size or fully transparent color skips the run.

Batching/flush (`Batched2DSink`): text is shaped with the caller's `TextShaper` and drawn through the
existing `TextRenderer`/glyph atlas path - no second text rasterizer. Glyph quads are textured quads
through `Batched2DRenderer::push_texture`, so a text run flushes the current solid/rounded batch when
the texture changes and then emits one batch per atlas page; rects after the run start a new batch.
The renderer's scissor and folded rounded-clip state apply to the glyph quads, so text is clipped
exactly like rects. Callers must call `Batched2DSink::begin_text_frame()` once per painted frame
(atlas frame bookkeeping); font, rasterizer, shaper, and atlas lifetime stay caller-owned. On an sRGB
target the glyph color follows the same linear-to-sRGB write conversion as rects; atlas coverage is
alpha, so it is unaffected.

Damage-clipped painting: the frame pipeline's damage decorator replays a text run once per
intersecting damage region under a square damage clip, keeping the run origin (baseline and pen
start), so glyph positions match the full paint and nothing outside the plan is repainted.

Tests: `paint_text` covers emission order/clip, precedence/opacity, empty/missing payloads, UTF-8
truncation and dirty domains, and damage-clipped replay; `paint_text_gpu` renders a fixture string
and asserts glyph coverage inside the box, background outside, byte-identical repeat, and an sRGB
target pass. No new image golden: the CPU canvas has no text rasterizer and the existing lavapipe
text goldens come from the direct `TextRenderer` path, so paint-text coverage stays a deterministic
GPU readback test (documented as GPU-only).

Limits/deferred: single-line text only (no wrapping, line breaking, bidi, or ICU); no rich text or
inline images; no text selection/editing; no per-glyph damage caching; font stack/fallback and
shaping options stay caller-owned; text does not affect layout (it paints in the node's existing
box); paint-text goldens are deferred until the CPU/software path rasterizes text deterministically.

### 10.1 Multi-line and wrapped text (task-39)

`PaintData` options: `wrap` (`PaintWrap::NoWrap` default = the V1 single-line behavior |
`PaintWrap::Wrap`), `max_lines` (0 = unlimited) and `text_align` (`nullopt` = use computed
`text-align`, falling back to Start; an explicit Start/Center/End overrides it). Precedence:
`wrap` is authoritative for paint text and computed `white-space: nowrap|pre` can only suppress
wrapping (never opt in); `max_lines` is PaintData-only; the resolved `text-align` applies to each
wrapped line inside the content box.

Layout reuse: wrapped lines come from the existing shared `text::layout_text` line-box path (same
greedy word wrapping used by measurement) - no new text engine, no ICU/bidi. Paint text uses the
deterministic `TextStyle` from computed style (`font-size`, `line-height`, `letter-spacing`,
`font-weight`, `font-style`, `white-space`) plus the PaintData font-size/font-weight overrides,
then `StubFontMetricsProvider` metrics; `TextLine` now also exposes the byte range of each line.
Paint stays pure: no scene/layout pass runs and PaintData never influences layout boxes.

Dimensions: the wrap width is the content box - border box minus `padding-left/right` (px only in
V1; percentage padding is treated as 0 and documented as a limitation). `line-height` comes from
computed style (px/number/%; `normal` uses ascent+descent+line-gap), and each line top advances by
its line-box height. NoWrap emits one `draw_text` with the content-box rect and the whole text;
Wrap emits one `draw_text` per line with `TextDraw::baseline` set to the absolute line baseline.
Line x positions follow Start/Center/End alignment (clamped to the content-box left edge). Line x
and baseline are rounded to whole pixels at paint time only; layout widths/offsets stay fractional
and no node bounds are modified.

Truncation: `max_lines = N` emits the first N computed whole lines (no partial words, no ellipsis
in V1); the result is deterministic for the same text+width+style. Hard newlines split paragraphs;
runs of whitespace collapse to one inter-word advance for wrapping while a line's byte range keeps
the original inner separators; words wider than the content box are split by UTF-8 code point.

Damage replay: the frame pipeline's damage decorator replays every emitted line under its square
damage clip (one clip push/pop per line per intersecting region); lines outside the plan are not
repainted, and a line keeps its origin relative to the full paint.

Caching/determinism: line boxes are a pure function of text + content width + style, so repeated
runs produce identical line boxes and pixels. The paint pass optionally uses a caller-owned bounded
cache (section 10.1.1): `PaintContext::text_layout_cache == nullptr` recomputes every paint (the
default, uncached path); a supplied `ui::paint::TextLayoutCache` reuses line boxes across frames.
No global singleton exists.

### 10.1.1 Paint text-layout cache (task-46)

`include/tessera/ui/paint/text_layout_cache.hpp` defines the caller-owned bounded LRU cache. The
paint frame/sink owns one instance and passes it through `paint_scene(..., PaintContext)`; the
context also carries a `text_metrics_source_id` (0 = the deterministic stub metrics paint uses
today). Callers that measure with a real face use a stable distinct id and `invalidate()` when the
face/generation changes.

Key (all fields, exact equality, floats compared by IEEE bit pattern, no epsilon): UTF-8 text
bytes, wrap width (content-box width; normalized to 0 when no-wrap so unused width does not split
the cache), font size, font weight, italic, letter spacing, line height, effective wrap mode,
`max_lines`, and the metrics-source id. Value: the line boxes (byte ranges, widths/heights,
baselines) already truncated to `max_lines`, plus the full-layout width/height/baseline.

Bounds and eviction: defaults are **64 entries / 256 KiB**, both enforced; exact byte accounting is
`sizeof(Entry) + 2 * (sizeof(Key) + text.capacity())` (map key + LRU key) `+ 2 pointers` (LRU node)
`+ line capacity * sizeof(TextLine)`. Entries are evicted least-recently-used first, so eviction is
deterministic for a fixed request sequence. A single entry larger than the byte budget is returned
through a scratch layout instead of breaking the bound. A hit performs no allocation (heterogeneous
`std::map` lookup with a string-view key, then an LRU-list splice) and returns a stable reference
until the next insert/evict/invalidate/clear. `invalidate()` drops everything (font/metrics change),
`clear()` empties without counting an invalidation, and `set_enabled(false)` bypasses the cache for
tests while returning identical fresh line boxes.

Benchmarks (`paint_text_layout_cache_tests --bench`, Debug, clang 22): the 200-word/300 px paragraph
(45 lines) costs 96.8 us uncached, 98.1 us for a miss+insert and 0.75 us for a cache hit (200
iterations averaged); the hit path removes the layout cost from steady-state painting.

Tests: `paint_text_layout_cache` covers hit/miss counters, per-key-field miss isolation (including
exact float-key bits), LRU eviction by both budgets, exact byte accounting, cached-vs-fresh line-box
equality, invalidate/no-cache behavior, paint integration (identical cached/uncached lines, second
paint hits), determinism across instances, and the `--bench` measurement.

Deferrals: glyph-run/shaping caches, incremental layout of an edited paragraph, and sharing the cache
with the layout engine (which measures with its own `TextMeasurer`).

### 10.1.2 Metrics sources: stub vs real font (task-53)

Paint wrapping measures through a documented `PaintTextMetricsSource`
(`include/tessera/ui/paint/paint_text_metrics.hpp`): `id()`, `metrics(style)`, and
`layout(text, style, constraints) -> TextLayout`. `PaintContext::text_metrics_source == nullptr`
selects `StubPaintTextMetrics` (id 0), which keeps today's deterministic stub metrics and
`text::layout_text` path, so existing CPU/GPU goldens and every paint/text test are byte-identical.
A caller that has a real face constructs `FontPaintTextMetrics(face, id)`: line metrics come from
`FreeTypeFontMetricsProvider` and wrap widths from the same HarfBuzz shaping advances the glyph
renderer uses (via `text::layout_text_measured`, the shared greedy line-box algorithm with a
measured-advance callback). The source id is part of the `TextLayoutCache` key, so stub and
real-font line boxes can never mix; switching faces means `invalidate()` plus a new stable id.

With real metrics, a line is never wider than the content box except when a single UTF-8 code point
is wider than the box; under-fill is bounded by the last word that did not fit plus one inter-word
advance. Alignment uses the measured line width and paint-time integer snapping is unchanged. Paint
emits the source's line boxes verbatim; the GPU test asserts paint lines equal
`FontPaintTextMetrics::layout`, center alignment and wrapped pixels.

Measurements (`paint_text_gpu_tests --bench`, Debug): stub ~200.8 us, real-metrics ~13.2 ms
(per-word HarfBuzz shaping), real-source cache hit ~1.0 us; faster real-metric measuring stays
deferred. Deferrals: contextual/ligature line breaking, bidi/ICU/hyphenation, subpixel positioning,
emoji/font-stack fallback and a shaping cache. Batching: one `draw_text` per line; consecutive lines
on the same atlas page stay in one renderer batch and a page/clip change flushes as before (§10).

Tests: `paint_text_wrap` (NoWrap vs Wrap, padding-box width, multi-line order/offsets/baselines,
`max_lines` truncation, alignment + computed fallback, `white-space: nowrap` suppression, damage
replay of multiple lines, env-free `--bench` measurement) and the extended `paint_text_gpu`
(wrapped fixture paragraph: several line bands inside the padded content box, background outside,
byte-identical repeat, sRGB target). No new image golden: the CPU canvas cannot rasterize text, so
wrapped-text pixels remain covered by the deterministic GPU readback test (GPU-only, documented).

Deferrals: bidi/SheenBidi, ICU line breaking, hyphenation, rich text/inline images, ellipsis and
overflow markers, text selection/editing, vertical content-box clipping (use an `overflow`
clip), and percentage padding.

## 11. Embedded vector assets (D10)

A node may paint one embedded SVG asset instead of text: `PaintData::kind == PaintKind::Vector`
with `PaintVectorStyle` (asset name borrowed; optional device-pixel raster size, uniform scale
hint, tint, and opacity multiplier). `PaintKind` selects the active payload - text fields are
ignored for a vector node and vice versa; `PaintData::empty()` follows the active kind. Names over
`kMaxAssetBytes` (256) are ignored, never truncated.

Emission mirrors text: after the node's background/borders, inside its own overflow/scroll clip,
before its children; `rect` is the padded content box in absolute coordinates. Node opacity folds
into the tint alpha together with the payload opacity (`tint.a * opacity`), so a vector composites
exactly like a text run. `PaintStats::vectors` counts emitted draws, and `PaintSink` gained an
additive default no-op `draw_vector(const VectorDraw&)` hook.

`Batched2DSink` implements the hook by constructor injection (`VectorPaintConfig`: generated asset
bundle, `SvgDocumentCache`, `PathTextureCache`, device scale, texture format matching the target
color space). One texture per (document, content, raster scale); N nodes sharing an asset cost one
decode and one upload. Missing, malformed, unsupported and ThorVG-OFF cases are documented silent
no-ops counted in `VectorPaintStats` - paint never crashes because an asset is absent - and
`ClippedPaintSink` replays vector draws per damage region exactly like glyphs. Full contract,
lifetime rules and measurements: [`thorvg-spike/10-paint-integration.md`](thorvg-spike/10-paint-integration.md).

### 10.2 Text decoration: caret and selection (task-58)

`PaintData::decoration` is an optional `TextDecoration` (caret byte offset, half-open selection
begin/end byte offsets, `caret_visible`, `blink_phase`). Paint consumes byte offsets only: it maps
them through the same metrics source and line boxes as the wrap path (`PaintContext::
text_metrics_source`, deterministic stub by default) and never reads the edit buffer. Newline bytes
belong to the preceding line end: a caret at a newline paints at that line's end (before the break),
a caret at the next line's first byte paints at that line's start, and a caret at text end paints
after the last glyph; offsets past the text clamp to the end. Selection intervals intersecting a
line paint from the measured prefix advance of the start boundary to the measured advance of the end
boundary; fully covered lines paint their full line width (a selected newline itself has no
rectangle).

Z-order: selection fills are emitted before the glyph runs and the caret fill after them. Both are
ordinary `PaintSink::fill_rect` calls, so node/scroll clips and damage-region replay clip them like
any other rect, and node opacity folds into their alpha. Selection color is the resolved text color
with alpha `0.35 * opacity`; caret color is the resolved text color at its existing alpha (node
opacity already folded), 1 px wide at device scale 1 and 2 px at scale >= 2
(`PaintContext::device_scale`). `caret_visible == false` or `blink_phase < 0.5` hides only the
caret; paint has no timers and the caller drives blinking. Absent decoration keeps previous output
byte-identical; a node with decoration emits per-line glyph runs (even NoWrap) so the decoration
geometry matches the emitted lines. Tests: `paint_text_decoration` (offset mapping
start/middle/end, multi-line and newline mapping, collapsed selection, z-order, width/colour/
opacity, clip and damage replay, blink, empty text), the `paint_text_gpu` decoration readback, and
the CPU text scene/golden (selection + caret). Deferrals: blinking timers, selection handles,
bidi/RTL carets, `::selection` style, sub-pixel caret positioning.

### 10.2.1 IME preedit (task-72)

`TextDecoration` carries the active composition descriptor: `composition_active`, half-open
`preedit_begin`/`preedit_end` buffer bytes (equal offsets = insertion there), `preedit_underline`
and the bounded `preedit_text`. The preedit is **not** part of `PaintData::text` while composing, so
paint never reads the edit controller; commit (`EditText`) or cancel (`CompositionEnd` without
commit) deactivates it.

- **Underline:** an empty `preedit_text` (composition already in the painted text) underlines the
  replaced range through the selection byte -> line mapping; otherwise the preedit run is underlined.
  Thickness is 1 px at device scale 1 and 2 px at scale >= 2; the colour is the resolved text colour.
- **Preedit run:** when `preedit_text` is non-empty, paint emits one `draw_text` run at the anchor
  using the measured prefix advance, the line's baseline, and the committed text style/colour. An
  empty committed text synthesizes one line box, so the preedit is visible before the first commit.
- **Z-order:** selection fill, glyph runs, preedit run, preedit underline, caret. Everything is an
  ordinary `PaintSink` call, so node/scroll clips and damage-region replay clip it and node opacity
  folds into the run/underline alpha. No new layout path and no sink hook.

Tests: `paint_text_preedit` (descriptor lifecycle, underline range only, 1/2 px thickness,
empty-buffer preedit run, commit/cancel removal, z-order, damage replay, opacity, determinism); no
PNG golden - the RecordingSink record is byte-exact and the GPU readback covers pixels. Deferrals:
candidate-window positioning, per-composition styling, bidi preedit, selection inside the preedit.

## 12. Raster image assets (D12)

A node may paint one embedded PNG/JPEG instead of text or a vector: `PaintData::kind ==
PaintKind::Image` with `PaintImageStyle` (borrowed asset name bounded by `kMaxAssetBytes`, optional
device-pixel raster size, scale hint, tint and opacity multiplier). `PaintKind` selects exactly one
active payload (text/vector/image are mutually exclusive); fields of the other kinds are ignored and
`PaintData::empty()` follows the active kind.

Emission and compositing mirror vectors: after the node's background/borders, inside its own
overflow/scroll clip, before its children; `rect` is the padded content box; effective tint alpha is
`tint.a * payload.opacity * effective opacity`; `PaintStats::images` counts draws. `PaintSink` gained
an additive default no-op `draw_image(const ImageDraw&)`, and `ClippedPaintSink` replays image draws
per damage region under the region clip exactly like glyphs and vectors.

`Batched2DSink` implements the hook with constructor injection (`ImagePaintConfig`: asset bundle,
`ImageDecodeCache`, `ImageTextureCache`, device scale, texture format matching the target color
space). One decode per content hash and one texture per (content, raster extent, format); missing,
corrupt, unsupported and unconfigured cases are silent no-ops counted in `ImagePaintStats` (paint
never crashes because an image is absent). PNG/JPEG bytes are authored sRGB, so the upload format
follows the target color space (the D2 rule); the full contract, caps, cache keys and measurements
are in [`thorvg-spike/12-image-assets.md`](thorvg-spike/12-image-assets.md).

### 10.3 Bidirectional visual-run emission

Empty `visual_runs` keeps the pre-bidi emission; bidi lines emit one `TextDraw` per visual run and
caret/selection use `bidi_offset_x` (`text.md`); RTL click-to-position is deferred.

### 13. Linear gradients (task-76)

`PaintData::gradient` (1-8 stops; offsets clamped/sorted, later duplicate wins; CSS angle 0=top,
90=right, 180=bottom) paints after `background-color`, before borders/content, inside border radius;
opacity folds into stop alphas. Interpolation is encoded-space sRGB (V1); emission is 64 dominant-axis
solid bands (exact on axes, banded otherwise; LUT/dithering follow-up), shared by CPU/GPU within
2/255 per channel. CPU rounds corners by bounds, GPU uses the rounded clip, damage replay is
band-identical. Deferrals: radial/conic, dithering, text/clip gradients, LUT.

## 14. Isolated-layer seam (issue #242)

The renderer can break its render pass without finishing the frame and copy one region of the frame
target into an offscreen target: `Batched2DRenderer::break_pass()`/`resume_pass()` plus the RHI
`CommandList::copy_target_region`, with a pooled target bounded by `glassMaxBlurTargets` (default 4).
`resume_pass()` reopens the same target with `LoadOp::Load`, so geometry before and after the break
composes exactly as on the single-pass path; a tree that never breaks a pass records byte-identical
commands and keeps today's goldens. This is the seam `backdrop-filter` (#244) uses for its backdrop
copy/blur, and the seam true isolated layers would use for group opacity (see
[§14.1](#141-effective-opacity-issue-227)); #227 itself ships the non-isolated rule below.

The #244 consumer is `PaintSink::draw_backdrop`, emitted once per glass node at the segment boundary
with the clamped sigma, rounded clip, folded opacity and color factors; high contrast drops it. The
composite draw sampling a pooled target is still deferred (`Batched2DSink` counts it unsupported);
contract: [`gpu/offscreen-layers.md`](gpu/offscreen-layers.md), [`gpu/rhi.md`](gpu/rhi.md) section 5.

### 14.1 Effective opacity (issue #227)

`opacity` is not inherited, but its visual effect composites the node's whole subtree. `paint_scene`
threads the product of the ancestor opacities down the recursion and folds it, with the node's own
value, into every primitive it emits: backgrounds, borders, box shadows, gradients, glass backdrops,
glyph quads and caret/selection fills, and vector/image tints. Overlapping siblings composite
separately, so this is **non-isolated** group opacity: correct for non-overlapping children and for
every fade, but not the isolated-layer result CSS specifies for overlapping content. `opacity: 0`
still walks the subtree and never touches the scene, so a transparent card stays hit-testable.

The ancestor product is part of the paint-cache key (a record never replays under another ancestor
opacity), and the frame pipeline marks an animated opacity node's descendants Paint|Render dirty, so
a parent fade repaints the subtree and its damage. True isolation (subtree into an offscreen target,
composited once) is the seam shared with shared-element snapshots and stacking contexts; **not**
implemented here: [`gpu/offscreen-layers.md`](gpu/offscreen-layers.md) section 5.2.