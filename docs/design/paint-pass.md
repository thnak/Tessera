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

`Node::bounds()` is parent-relative ([`node.hpp`](../../include/tessera/ui/scene/node.hpp)). The pass
therefore paints at `world_bounds()` plus the `bounds()` origin of every ancestor:

```text
paint_rect(node) = world_bounds(node) + sum(bounds(ancestor).xy for ancestor in parents)
```

- Identity and translation-only transforms are exact.
- `world_bounds()` is the axis-aligned box of the four transformed corners; rotation/scale are
  approximated by that AABB plus the ancestor origins. Composing per-node transform order is out of
  scope for this slice.
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
