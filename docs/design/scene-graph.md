# Scene graph and event dispatch

## Node model

`tessera::ui::Node` is a retained tree node:

- `NodeId id`
- local `rhi::Rect bounds`
- local-to-parent `rhi::Mat3 transform`
- children in **paint order** (the last child is topmost)
- `bool visible`
- virtual `on_event(Event&)`

Derived values:

- `world_transform()` = `parent.world_transform() * transform`
- `world_bounds()` = axis-aligned bounding box of the four transformed corners

There is no implicit clipping in the scene graph. Clipping is a paint concern handled by the
batched 2D renderer's scissor stack.

## Dirty state

Each node carries dirty bits:

- `StyleDirty` — class/id/attribute/pseudo-state/theme/inline-style changed.
- `LayoutDirty` — a layout-affecting style change, child add/remove, text/font change, or resize.
- `PaintDirty` — paint-affecting style change, transform/clip/visibility, layout move/resize,
  texture/atlas change.
- `RenderDirty` — any paint change, target resize, damage, or explicit request.

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
- Parent clipping and rounded-rect hit regions are deferred to B2.

## Events

Event types in V1: `PointerDown`, `PointerUp`, `PointerMove`.

Dispatch is W3C-style:

1. hit-test to find the target;
2. build the path from root to target;
3. **capture** phase: root → target's parent;
4. **target** phase: target;
5. **bubble** phase: target's parent → root.

`Event::stop_propagation()` stops the remaining handlers immediately. `handled` is advisory and
does not stop propagation by itself.

The dispatcher is UI-thread-only. No locking, no async delivery, no event pooling yet.

## Tests

- `tests/unit/scene_graph_tests.cpp`: world bounds, deep hit testing, capture/target/bubble
  order, stop propagation, invisible subtree skipping, outside-bounds misses.
- `tests/unit/batched2d_tests.cpp`: batch count and vertex count across texture and scissor
  changes, with the null backend.

## B2 direction

- Yoga adapter produces `bounds` from flex/grid styles.
- Full dirty-bit plumbing into style/layout/paint caches.
- Focus, keyboard, pointer capture, hover/active state, and IME.
- Accessibility tree mirroring the node tree.
- Rounded-rect/path clip regions in the renderer, with matching hit testing.
