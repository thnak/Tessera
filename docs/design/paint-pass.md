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
-> paint -> render). Damage/redraw (see [`dirty-rendering.md`](dirty-rendering.md)) is unchanged: the
pass can paint a full frame, and wiring it to `RedrawPlan` scissors is a later slice.
[`FramePipeline::paint_frame`](#8-frame-integration) is what invokes it once per due frame.

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
- There is no clipping: children paint over their parent's box, as in the scene graph's paint order.

## 4. Property mapping

| Property | Use |
|---|---|
| `background-color` | fills the node's paint rect |
| `border-{top,right,bottom,left}-width` | edge thickness in px |
| `border-{top,right,bottom,left}-style` | `none`/`hidden` (or missing) skips the edge; any other keyword paints solid |
| `border-{top,right,bottom,left}-color` | edge color; `currentColor` uses `color` |
| `border-*-radius` | when all four are equal and > 0, the background is one rounded rect |
| `color` | inherited `currentColor` base; invalid/missing falls back to black |
| `opacity` | multiplies the alpha of every color the node emits (clamped 0..1) |

Parsing rules:

- Lengths accept `px` and bare numbers; percentages, `em`, and other units are unsupported and the
  property is ignored. Missing lengths use the property's initial value (0 for widths/radii).
- Colors use [`style::parse_color`](../../src/ui/style/color.cpp), so named/hex/rgb/rgba/hsl work.
  `transparent` and any zero-alpha color are skipped, as are unparseable values.
- `opacity` applies to the node's own background and borders; it does not isolate a group or
  composite children.

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

- No text, glyphs, or inline content; text has its own renderer.
- No clipping/overflow, scroll, masks, filters, gradients, shadows, or images.
- No damage/redraw plan or dirty-diff: every call repaints the visible subtree it is given.
- No layer isolation or group opacity; `opacity` is per emitted color.
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
scene, the `SceneStyleProvider` path, and a null-backend integration that asserts two draws and that
paint never submits the command list (the caller does).

Determinism: no clock, fonts, network, or GPU; colors and rects are compared exactly (alpha within a
small epsilon), and the recorder captures emission order.

## 8. Frame integration

[`FramePipeline`](../../include/tessera/ui/frame_pipeline.hpp) owns the per-frame order and exposes
the paint stage through one bounded call:

```cpp
FramePaint FramePipeline::paint_frame(const Node& root, paint::StyleProvider& styles,
                                      paint::PaintSink& sink, bool force = false) const;
```

`FramePaint` carries the `paint::PaintStats` and a `painted` flag, so "frame was not due" is
distinguishable from "painted an empty subtree" without inspecting the sink.

Frame order per iteration:

```text
tick(root, dt)          # animation samples, dirty collection, needs_style/needs_layout
  (caller: style re-resolve / layout if the tick asked for it)
finish_frame(root)      # damage + normalized redraw plan -> should_render
paint_frame(root, styles, sink)   # paints only when should_render, or when force is set
  (caller: submit the command list / present)
```

Contract:

- **Only due frames paint.** `finish_frame()` records `FrameRender::should_render` as the pipeline's
  frame decision; `paint_frame()` is a no-op (`painted == false`, no stats, no sink calls) when the
  frame is not due. `tick()` clears the decision, so a new frame cannot paint until its own
  `finish_frame()`. The explicit `force` flag paints a full frame anyway (for example after a resize
  or a backend reset); it does not change the frame decision.
- **Full-frame only.** The paint pass ignores `FrameRender::redraw_plan`: even a partial redraw
  repaints every visible box it is given. Damage-clipped painting (scissoring the sink to the plan's
  `Scissor` regions) is a later slice. A caller that needs partial output clips at the
  sink/renderer level; the scene and the plan stay untouched.
- **Read-only scene.** `paint_frame()` takes `const Node&`; it does not mark or clear dirty flags,
  does not queue damage, and does not touch `DamageTracker`/`DamageAccumulator`. Style and layout
  decisions (`FrameTick::needs_style`/`needs_layout`) keep their existing semantics.
- **Caller owns the inputs.** The stylesheet/style provider and the sink are supplied by the caller,
  exactly as for `paint_scene`; the pipeline adds no backend, GPU, or device dependency.
  `paint::SceneStyleProvider` over a compiled stylesheet and a `SceneStyleTree` is the default
  provider; tests use a recording sink and a stub provider.
- **Repeatable.** Painting twice in one due frame is allowed and deterministic: the same commands
  and the same `PaintStats` are produced.

The frame-pipeline tests in
[`tests/unit/frame_pipeline_tests.cpp`](../../tests/unit/frame_pipeline_tests.cpp) cover: no paint
before a frame decision, no paint on an idle frame, an animation-dirty frame painting once with the
expected background/border rects in paint order, stats (`nodes`/`backgrounds`/`borders`) matching
the painted scene, a scene snapshot (bounds, transform, visibility, dirty flags, child counts)
unchanged across a paint, repeated paints producing identical commands, forced painting of an idle
frame, and painting neither queueing damage nor changing the next frame decision.
