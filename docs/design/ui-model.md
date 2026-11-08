# UI model: components, scene graph, and the frame

Status: design for Big Step B2 (runtime slice) and Big Step C (compiler vertical slice); both are defined in
[AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step). The B1 (Big Step B phase 1,
renderer core) scene graph is implemented; components/hooks/styles are not. Terms (IR, RHI, Yoga,
ComputedStyle, protected region, WindowMetrics) are in [`glossary.md`](../reference/glossary.md).

Tessera combines a **declarative component model** (JSX/React syntax at design time) with a
**retained scene graph** at runtime. The compiler lowers components to C++ that constructs and
updates scene nodes. There is no React runtime in production.

## 1. Two representations

| Representation | Lifetime | Owner |
|---|---|---|
| Component tree (JSX/TSX) | Design time / generated C++ structure | Compiler + generated C++ |
| Scene graph (`ui::Node`) | Runtime | UI thread |
| Display list / batches | Per frame or retained | Renderer |

The component tree describes **what**; the scene graph is the retained **what exists now**;
the renderer consumes an immutable draw list. Styles are computed on scene nodes, not on
component objects.

## 2. Component contract (V1)

V1 = the first release; its scope rule is [AGENTS.md Non-negotiables](../../AGENTS.md#non-negotiables)
item 8 (small, explicit hooks subset, unsupported patterns rejected loudly).

A component has:

- a name and typed props (from JSX attributes)
- optional children/slots
- state hooks: `useState`
- effects: `useEffect(fn, deps)`
- refs: `useRef(null)` (points to a scene node)
- animated values: `useAnimatedValue(initial)`
- event handlers for pointer/keyboard/focus/change
- a returned node tree (`View`, `Text`, `Button`, `TextInput`, `TextArea`, `Image`, `Path`,
  `Scroll`, `VirtualList`, `VirtualGrid`, `CustomElement`)

V1 rejects: custom hooks, context, suspense, dynamic component types, portals, and non-static
conditionals/loops. Portals are replaced by the explicit overlay host/service
([`overlays-and-layers-v1.md`](../specs/overlays-and-layers-v1.md)). Error boundaries are
supported wrappers; see
[`component-model-v1.md`](../specs/component-model-v1.md) and
[`error-boundaries-v1.md`](../specs/error-boundaries-v1.md).

`CustomElement` is an intrinsic native-render leaf for custom shaders/3D. It is declared with a
static `element` id and implemented in C++ using the RHI; it is not a dynamic component type.
See [`custom-elements-v1.md`](../specs/custom-elements-v1.md).

## 3. Build-time lowering

```text
Component.tsx
  -> Tessera JSX/TSX front end ([ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md))
  -> Tier 1 React rules (hooks order, purity, valid JSX; see compiler.md)
  -> Tier 2 bindings (`// Note: bind::<id>`; see compiler.md)
  -> custom element manifest/registry (`ui/elements.json`)
  -> IR (docs/specs/ir-v1.md; neutral intermediate representation)
  -> generated C++:
       Component::build(Node& parent) / Component::update()
       state members + setters
       effect/lifecycle stubs
       protected developer regions
```

The compiler design is in [`compiler.md`](compiler.md) and the IR in [`ir-v1.md`](../specs/ir-v1.md).
Generated C++ is allowed to construct scene nodes and call runtime APIs. It must not parse
JSX, execute JavaScript, or depend on the compiler at runtime.

## 4. Scene graph

`tessera::ui::Node` is a retained node:

- stable `NodeId`
- local `Rect bounds`
- local-to-parent `Mat3 transform`
- children in paint order (last child is topmost)
- `visible`
- style/dirty state (Big Step B2)
- semantics/accessibility data (data model in B2; platform bridges in Big Step F)
- virtual `on_event(Event&)`

Derived values:

```text
world_transform = parent.world_transform * transform
world_bounds    = AABB of the four transformed corners
```

There is no implicit clipping in the scene graph; clipping is a paint concern (scissor stack).
Hit testing inverts the world transform and tests local bounds. Deep hit testing visits
children topmost-first, then the node itself.

## 5. Identity and reconciliation

- A component instance owns a stable identity within its parent slot.
- Keys identify list items; stable keys preserve state/focus/scroll.
- Reordering without keys is allowed only for static lists; dynamic lists require keys.
- The generated C++ updates properties/children in place; it does not recreate the tree every
  frame.
- Removed nodes are detached and retired after the current frame; GPU resources follow the
  resource lifetime rules.
- Text/content changes mark style, text measurement, layout and paint dirty (see [`dirty-rendering.md`](dirty-rendering.md) section 3).

V1 lists are explicit runtime list models; the compiler rejects dynamic `.map()` expressions
that cannot be statically analyzed. Large collections use `VirtualList`/`VirtualGrid`; see
[`virtualization-v1.md`](../specs/virtualization-v1.md) for windowing/recycling rules.

Custom elements are keyed/reused like any component; shared pipelines/meshes/textures live in
the resource cache, while per-instance renderer state is retired after frames in flight.

## 6. Style, layout, paint, animation

Order per tick (the loop itself is in [`game-loop.md`](game-loop.md) section 3):

```text
input/pseudo-state -> style -> layout -> animation -> paint -> render
```

- **Style**: selector matching, cascade, custom properties -> `ComputedStyle`.
- **Layout**: Yoga/Flexbox + grid subset -> bounds.
- **Animation**: fixed-step sampling -> animated values written into computed style before
  layout/paint.
- **Paint**: draw list/batches from style + bounds + text.
- **Render**: RHI (render hardware interface) command list; a stage is skipped when its dirty domain is clean.

Each stage has a dirty bit; see [`dirty-rendering.md`](dirty-rendering.md). A style change only
raises layout/paint when the relevant computed group changed.

## 7. Events and focus

- Pointer: hit test -> capture/target/bubble (W3C-style).
- Keyboard: focused node -> bubble; shortcuts may be registered as global (the registration API is not specified here; see [`input.md`](input.md)).
- Pointer capture: a node can capture a pointer until release/cancel.
- Hover/active/focus/focus-visible feed the style engine before style resolution.
- IME (input method editor) composition is routed to the focused text input (B2).
- Events are dispatched on the UI thread; no async delivery and no locks.
- Event handlers may mutate state; layout/paint are deferred to the frame boundary.

See [`input.md`](input.md) and [`scene-graph.md`](scene-graph.md).

## 8. Responsive and foldable UI

- The UI sees `WindowMetrics` (size, DPI/scale, safe/work area, orientation, fold).
- Breakpoints/media queries and container queries adapt layout; see
  [`responsive.md`](responsive.md).
- Fold posture is a metric, not a separate component mode: tabletop -> top content/bottom
  controls; book -> left/right panes; avoid `occlusion=FULL`/`separating` hinge areas (fold fields are defined in [`os-events-v1.md`](../specs/os-events-v1.md)).
- Orientation/fold changes preserve state and focus; the compiler-generated component is not
  recreated unless the platform forces a restart (Android fold/unfold; see
  [`mobile-integration.md`](mobile-integration.md) section 3.5).

## 9. Text and accessibility

- Text nodes own shaped runs and glyph quads; layout uses measure callbacks.
- Every interactive node owns a semantics entry (role/name/state/actions).
- The accessibility (semantics) tree mirrors the scene tree and updates when a node's semantics change (see [`accessibility.md`](accessibility.md)).
- Text scale, high contrast, reduced motion, and screen-reader state are inputs, not global
  flags mutated by user code.

## 10. Ownership and lifetime

- Components own their state; nodes own children; the UI thread owns the tree.
- `useRef` points to a node but does not own it.
- Effects run after commit, on the UI thread, and must not block.
- Async work returns through the runtime completion queue; it never touches nodes directly.
- GPU resources are owned by the device; scene nodes reference handles, not raw GPU objects.
- Destruction order: detach nodes -> retire GPU resources after frames in flight -> destroy
  device.

## 11. Minimal generated shape (illustrative)

```cpp
// Generated; do not edit outside protected regions.
struct Counter {
    int count_ = 0;
    Node* label_ = nullptr;

    void build(Node& parent) {
        auto root = std::make_unique<Node>(next_id());
        // style/layout props omitted
        auto label = std::make_unique<Node>(next_id());
        label_ = label.get();
        root->add_child(std::move(label));
        parent.add_child(std::move(root));
    }

    void on_click(Event&) {
        count_ += 1;
        // request dirty style/text/layout
    }

    // [DEVELOPER_LOGIC_START: bind::on_click]
    // [DEVELOPER_LOGIC_END: bind::on_click]
};
```

The real codegen owns node creation, style binding, and dirty marking; developer logic is only
inside protected regions.

## 12. Tests

- Scene graph: bounds/transforms/hit testing/event order/stop propagation (B1).
- Component lowering: IR snapshots, generated C++ compile tests.
- Reconciliation: identity, keyed reorder, state/focus preservation.
- Style/layout/paint integration: golden scenes at fixed DPI/clock.
- Responsive/fold: synthetic metrics sequences.
- Accessibility: semantics snapshots and focus order.

## 13. Status

- B1: scene graph, hit testing, pointer event dispatch.
- B2: styles, Yoga layout, text, animation, input/focus, dirty rendering.
- C: component compiler, hooks, reconciliation, generated C++.
- F: accessibility bridges and production services.
