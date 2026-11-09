# Scene graph and event dispatch

## Node model

`tessera::ui::Node` is a retained tree node:

- `NodeId id` — runtime instance (uint64)
- `UiId ui_id` — build-time stable automation/trace id (uint64)
- `ComponentId component_id` — owning component (uint64)
- parent-relative `rhi::Rect bounds` (layout writes the box offset here)
- box-local `rhi::Mat3 transform` (animation/rotation, applied around the box origin)
- children in **paint order** (the last child is topmost)
- `bool visible`
- virtual `on_event(Event&)`

Derived values:

- `world_transform()` = `parent.world_transform() * translate(bounds.x, bounds.y) * transform`
- `world_bounds()` = axis-aligned bounding box of the transformed box-local rect
  `(0, 0, bounds.width, bounds.height)`

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

Nodes currently expose pseudo-state bits (`style::PseudoState`) and tab order (`tab_index`).
`ui/style/scene_style.hpp` materializes a `StyleNode` view from a scene subtree for selector
matching, including parent/sibling structure and pseudo-state; tag, id, class, attribute, inline
style, and theme scope bindings arrive with the element layer. The input dispatcher updates
`:focus` (and, later, `:hover`/`:active`/`:focus-visible`) before style resolution; those changes
mark `StyleDirty`. `SceneStyleTree::compute` refreshes pseudo-state from the node on each
evaluation, so focus/blur is visible to the next resolution. The style resolver raises
layout/paint/render dirty only when the matched computed groups actually change.

## Hit testing

- `hit_test(x, y)` inverts the world transform, maps the point into box-local space, and tests the
  rect `(0, 0, bounds.width, bounds.height)` (the bounds origin is part of the world transform).
- Singular transforms return no hit.
- `hit_test_deep(x, y)`:
  1. return null if the node is invisible;
  2. visit children in reverse order (topmost first);
  3. finally test the node itself.
- Parent clipping and rounded-rect hit regions are deferred to B2 (runtime slice of [Big Step B](../../AGENTS.md#big-steps-group-work-dont-micro-step)).

## Events

Event types in V1 (first-release scope, see [AGENTS.md rule 8](../../AGENTS.md#non-negotiables)):
`PointerDown`, `PointerUp`, `PointerMove`, `PointerOver`, `PointerOut`, `PointerEnter`,
`PointerLeave`, `KeyDown`, `KeyUp`, `Focus`, `Blur`. `PointerOver`/`PointerOut` bubble; the
non-bubbling `PointerEnter`/`PointerLeave` pair fires once per node on the entered/left chain.
Focus, keyboard, and hover-boundary routing are described in [`input.md`](input.md).

Dispatch is W3C-style (the DOM event model of the World Wide Web Consortium):

1. hit-test to find the target;
2. build the path from root to target;
3. **capture** phase: root → target's parent;
4. **target** phase: target;
5. **bubble** phase: target's parent → root.

`Event::stop_propagation()` stops the remaining handlers immediately. `handled` is advisory and
does not stop propagation by itself.

The dispatcher is UI-thread-only. No locking, no async delivery, and no event pooling (not implemented; no plan is written down).

## 5. Element metadata

`Node` stores additive, allocation-free element metadata for codegen and future selector/accessibility
use: `set_element_kind`/`element_kind` (borrowed view, byte cap `kMaxMetadataBytes = 256`),
`set_element_id`/`clear_element_id` and `set_class_name`/`clear_class_name` (borrowed views; setting
or clearing marks `Dirty::Style` and is a no-op when unchanged), `set_automation_id`/`automation_id`
(`std::uint64_t`, 0 means unset), `set_role`/`clear_role`, and `set_aria(name, value)`/
`clear_aria`/`aria_attributes()` (fixed cap `kMaxAriaPairs = 16`; name and value caps 32/128 bytes;
replacing an existing name updates it in place; a full cap or an empty name returns false, it never
throws). There are no platform calls: the accessor is the seam the future accessibility bridge
consumes.

Ownership: every metadata string is a **borrowed `std::string_view`**; `Node` copies nothing, so the
caller keeps the storage alive for as long as the node is inspected. Generated code passes string
literals (static storage), and dynamic metadata needs a runtime interning API (not in this slice).
Values are truncated to the documented byte caps (metadata is treated as ASCII identifiers/ARIA
values; no UTF-8 boundary logic). `id` and `className` are the selector inputs and mark
`Dirty::Style`; element kind, role, `aria-*` and `automationId` are metadata only and mark no dirty
domain. Text is not a metadata field: generated text attaches the existing D9 `PaintData` through
`set_paint_data`, and layout never reads that payload.

## Tests

- `tests/unit/scene_graph_tests.cpp`: world bounds, deep hit testing, capture/target/bubble
  order, stop propagation, invisible subtree skipping, outside-bounds misses.
- `tests/unit/focus_tests.cpp`: focus transitions, invalid focus, Tab order/wrap, keyboard
  routing to the focused node, pointer-down focus, and focus cleanup on router reset.
- `tests/unit/scene_dirty_tests.cpp`: dirty-domain marking, ancestor aggregation, clear/recompute,
  setter invalidation, add-child propagation.
- `tests/unit/node_metadata_tests.cpp`: element kind/id/className/role/aria/automationId get/set/
  clear, Dirty::Style no-op semantics, borrowed-view byte caps and the aria pair cap.
- `tests/unit/codegen_runtime_tests.cpp`: builds the generated sample component and asserts the
  attached metadata, children, focusability and D9 text (no GPU).
- `tests/gpu/batched2d/batched2d_tests.cpp`: batch count and vertex count across texture and
  scissor changes, with the null backend.

## B2 direction

- Yoga adapter (Flexbox layout engine, see [glossary](../reference/glossary.md); [`layout.md`](layout.md)) produces `bounds` from flex/grid styles.
- Full dirty-bit plumbing into style/layout/paint caches.
- Focus, keyboard, pointer capture, hover/active state, and IME (input method editor); see [`input.md`](input.md).
- Accessibility tree mirroring the node tree ([`accessibility.md`](accessibility.md)).
- Rounded-rect/path clip regions in the renderer, with matching hit testing.
