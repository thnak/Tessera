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

`Scroll` is a scene event, never an OS event: the platform produces `Wheel`, and the scene emits
`Scroll` to a scroll container whose offset actually changed (wheel-driven, `scroll_to`/`scroll_by`,
`scroll_into_view`). It dispatches in the same capture → target → bubble order with the container as
the target; `scroll_x`/`scroll_y` carry the delta the container applied (web sign, positive
`scroll_y` scrolls content down) and `ScrollEventInfo` the resulting offsets plus the container's
scrollport/content metrics. A clamped-to-zero, non-scrollable, or no-op request emits nothing, and
`prevent_default` cannot undo a move that already happened (see
[`scroll-and-gestures-v1.md`](../specs/scroll-and-gestures-v1.md) section 2.1).

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

`style::SceneStyleTree` exposes that metadata to the selector matcher: `tag` comes from
`element_kind()` (a component instance host reports its component name instead), `id` from
`element_id()`, `classes` from the whitespace-split `class_name()`, `part` from `part()`, the
`::part()` boundary from the nearest ancestor with a component name, and `has_text` from a
`PaintKind::Text` payload. A compiled stylesheet therefore styles generated components by type,
class and id selector without an extra element layer, and reaches a component's inner elements
through `::part()` ([`stylesheet-v1.md`](../specs/stylesheet-v1.md) section 6.3); `aria-*` is not a
selector input (attributes stay empty). Rebuild the tree when children are added or removed.

`set_part`/`clear_part` and `set_component_name`/`clear_component_name` add the component-library
metadata ([ADR-047](../../decisions/ADR-047-component-library-layers-and-design-language.md) item 3).
Both are borrowed views behind one lazily allocated record, so a plain `View` pays a pointer and
`sizeof(Node)` stays inside `kNodeSizeBudget`. `part` (the attribute a component's inner element
declares) marks `Dirty::Style`; the component name types a host for matching, so changing it marks
the subtree.

## 6. Event payloads and generated bindings

Generated components attach handlers to scene nodes instead of subclassing them. Payload record
types live in `include/tessera/ui/scene/event_payloads.hpp` and are trivially copyable, RTTI-free
and allocation-free; string fields are borrowed views into the dispatcher's `Event` and are only
valid during dispatch. Mapping (schema/IR name → C++ type): `PointerEvent` → `PointerEventInfo`,
`ClickEvent` → `ClickEvent`, `KeyEvent` → `KeyEventInfo`, `FocusEvent` → `FocusEventInfo`,
`TextChangeEvent` → `TextChangeEvent` (`TextSubmitEvent` is the same record),
`TextSelection` → `TextSelection`, `CompositionEvent` → `CompositionEvent`,
`ScrollEvent` → `ScrollEventInfo`, `ErrorInfo` → `ErrorInfo`. Fields come from the payload record
definitions; fields the scene `Event` does not carry yet are zero/empty (`KeyEventInfo::code`,
`ErrorInfo`) and `KeyEventInfo::key` is the code point in this slice. `ScrollEventInfo` is filled
from the container's live state when the scene emits `EventType::Scroll`: `offset_x`/`offset_y` are
the resulting offsets, the viewport is the scrollport (border box inset by `scrollport_insets`), and
the content extents are the node's content size.

`Node::bind_event(EventType, handler, context)` stores at most `kMaxEventBindings = 8`
`{EventType, function pointer, void* context}` tuples per node with no per-event allocation; a
ninth binding returns `false`. `unbind_event` removes the first binding for a type and
`clear_event_bindings` removes all; binding and unbinding mark no dirty domain. `on_event` user
listeners run first at the Target phase, then `run_event_bindings` invokes every binding registered
for `event.type` in registration order, then bubbling continues. A binding receives a read-only
`const Event&`, so it cannot consume the event or change `handled`/`prevent_default`; the route log
is identical when no binding is installed. `Click` is the one event whose bindings also run on every
ancestor while it bubbles (see [`click-synthesis.md`](click-synthesis.md)).

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
  attached metadata, children, focusability, D9 text and a synthetic dispatched event running the
  generated setter (no GPU).
- `tests/unit/scene_event_binding_tests.cpp`: payload field mapping, the per-node binding cap,
  Target-phase ordering, read-only bindings and route-log equality without bindings.
- `tests/unit/scene_scroll_event_tests.cpp`: one `Scroll` per applied move with the delta/offsets and
  real viewport/content metrics, capture → target → bubble order, no event for clamped,
  non-scrollable or no-op requests, `scroll_into_view` emission, and the bounded self-scrolling
  handler.
- `tests/gpu/batched2d/batched2d_tests.cpp`: batch count and vertex count across texture and
  scissor changes, with the null backend.

## B2 direction

- Yoga adapter (Flexbox layout engine, see [glossary](../reference/glossary.md); [`layout.md`](layout.md)) produces `bounds` from flex/grid styles.
- Full dirty-bit plumbing into style/layout/paint caches.
- Focus, keyboard, pointer capture, hover/active state, and IME (input method editor); see [`input.md`](input.md).
- Accessibility tree mirroring the node tree ([`accessibility.md`](accessibility.md)).
- Rounded-rect/path clip regions in the renderer, with matching hit testing.
