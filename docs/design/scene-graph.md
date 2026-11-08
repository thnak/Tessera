# Scene graph and event dispatch

## Node model

`tessera::ui::Node` is a retained tree node:

- `NodeId id` — runtime instance (uint64)
- `UiId ui_id` — build-time stable automation/trace id (uint64)
- `ComponentId component_id` — owning component (uint64)
- local `rhi::Rect bounds`
- local-to-parent `rhi::Mat3 transform`
- children in **paint order** (the last child is topmost)
- `bool visible`
- virtual `on_event(Event&)`

Derived values:

- `world_transform()` = `parent.world_transform() * transform`
- `world_bounds()` = axis-aligned bounding box of the four transformed corners

There is no implicit clipping in the scene graph. Clipping is a paint concern handled by the
batched 2D renderer's ([`renderer.md`](renderer.md)) scissor stack (axis-aligned GPU clip rectangles).

`UiId`/`ComponentId` are stable build-time IDs for coding agents/traces; `NodeId` is the live
instance. List/grid items are identified by `(UiId, ItemKey)`. Snapshot/query APIs and the
manifest are defined in [`automation-ids-v1.md`](../specs/automation-ids-v1.md).

A `CustomElement` node is a leaf that owns a native renderer; the scene graph forwards layout,
input, and paint/render work to it. Offscreen/inline composition and input are defined in
[`custom-elements-v1.md`](../specs/custom-elements-v1.md).

## Dirty state

Each node carries dirty bits (invalidation flags; see "Dirty domain" in the [glossary](../reference/glossary.md)):

- `StyleDirty` — class/id/attribute/pseudo-state/theme/inline-style changed.
- `LayoutDirty` — a layout-affecting style change, child add/remove, text/font change, or resize.
- `PaintDirty` — paint-affecting style change, transform/clip/visibility, layout move/resize,
  texture/atlas change.
- `RenderDirty` — any paint change, target resize, damage (screen region to redraw), or explicit request.

Marking walks ancestors and sets `has_dirty_descendant`; it does not walk the whole subtree
unless the change is subtree-affecting (`display:none`, visibility, transform/clip, theme scope).
The runtime processes `style -> layout -> paint -> render` and clears each bit after the stage.
See [`dirty-rendering.md`](dirty-rendering.md).

## Style and input state

Nodes expose a `StyleNode` view for selector matching: tag, id, classes, attributes, parent and
sibling order, inline style, theme scope, and pseudo-state bits. The input dispatcher updates
`:hover`, `:active`, `:focus`, and `:focus-visible` before style resolution; those changes mark
`StyleDirty`. The style resolver raises layout/paint/render dirty only when the matched computed
groups actually change.

## Hit testing

- `hit_test(x, y)` inverts the world transform, maps the point into local space, and tests
  `bounds.contains(local_x, local_y)`.
- Singular transforms return no hit.
- `hit_test_deep(x, y)`:
  1. return null if the node is invisible;
  2. visit children in reverse order (topmost first);
  3. finally test the node itself.
- Parent clipping and rounded-rect hit regions are deferred to B2 (runtime slice of [Big Step B](../../AGENTS.md#big-steps-group-work-dont-micro-step)).

## Events

Event types in V1 (first-release scope, see [AGENTS.md rule 8](../../AGENTS.md#non-negotiables)):
`PointerDown`, `PointerUp`, `PointerMove`, `KeyDown`, `KeyUp`, `Focus`, `Blur`. Focus and keyboard
routing are described in [`input.md`](input.md).

Dispatch is W3C-style (the DOM event model of the World Wide Web Consortium):

1. hit-test to find the target;
2. build the path from root to target;
3. **capture** phase: root → target's parent;
4. **target** phase: target;
5. **bubble** phase: target's parent → root.

`Event::stop_propagation()` stops the remaining handlers immediately. `handled` is advisory and
does not stop propagation by itself.

The dispatcher is UI-thread-only. No locking, no async delivery, and no event pooling (not implemented; no plan is written down).

## Tests

- `tests/unit/scene_graph_tests.cpp`: world bounds, deep hit testing, capture/target/bubble
  order, stop propagation, invisible subtree skipping, outside-bounds misses.
- `tests/unit/focus_tests.cpp`: focus transitions, invalid focus, Tab order/wrap, keyboard
  routing to the focused node, pointer-down focus, and focus cleanup on router reset.
- `tests/unit/scene_dirty_tests.cpp`: dirty-domain marking, ancestor aggregation, clear/recompute,
  setter invalidation, add-child propagation.
- `tests/gpu/batched2d/batched2d_tests.cpp`: batch count and vertex count across texture and
  scissor changes, with the null backend.

## B2 direction

- Yoga adapter (Flexbox layout engine, see [glossary](../reference/glossary.md); [`layout.md`](layout.md)) produces `bounds` from flex/grid styles.
- Full dirty-bit plumbing into style/layout/paint caches.
- Focus, keyboard, pointer capture, hover/active state, and IME (input method editor); see [`input.md`](input.md).
- Accessibility tree mirroring the node tree ([`accessibility.md`](accessibility.md)).
- Rounded-rect/path clip regions in the renderer, with matching hit testing.
