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
- Focus, keyboard, pointer capture, and hover states.
- Accessibility tree mirroring the node tree.
- Rounded-rect/path clip regions in the renderer, with matching hit testing.
