# Component model v1

Status: proposed (Big Step C, defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step)). "V1" means the first-release scope ([AGENTS.md](../../AGENTS.md), Non-negotiables rule 8). Normative for component semantics and generated C++. The source syntax that is accepted is defined in [`jsx-subset-v1.md`](jsx-subset-v1.md).

A Tessera component is a pure description of UI state and a node tree. It is compiled to C++;
there is no React runtime in production ([AGENTS.md](../../AGENTS.md) Non-negotiables rule 7).

## 1. Component shape

```tsx
type CounterProps = { initial?: number };

export function Counter({ initial = 0 }: CounterProps) {
  const [count, setCount] = useState(initial);
  const label = useRef<Text>(null);

  // Note: bind::on_click
  const onClick = () => setCount(count + 1);

  return (
    <View className="counter">
      <Text ref={label}>Count: {count}</Text>
      <Button onClick={onClick}>Increment</Button>
    </View>
  );
}
```

Rules:

- Components are functions returning JSX.
- Props are typed; defaults are static literals.
- Hooks are called unconditionally at the top level, in stable order.
- Event handlers may set state; render must be pure.
- No custom hooks, context, suspense, portals, or dynamic component types.
- No arbitrary JavaScript execution at runtime; only generated C++.

## 1.1 Identity

- Each component has a stable `ComponentId` (`uint64_t`).
- Each UI node has a stable `UiId` (`uint64_t`); runtime `NodeId` is separate.
- An optional `automationId="<uint64>"` pins a stable target across moves/renames.
- List/grid items use `(UiId, ItemKey)`, not per-instance `UiId`.
- The compiler emits `generated/automation.manifest.json`; JSON uses decimal strings.
- IDs support coding agents and tracing; see
  [`automation-ids-v1.md`](automation-ids-v1.md).

## 2. Hooks

| Hook | V1 semantics | Generated C++ |
|---|---|---|
| `useState(init)` | state + setter, batched updates | member + setter; marks the node dirty ([`dirty-rendering.md`](../design/dirty-rendering.md)) |
| `useEffect(fn, deps)` | run after commit when deps change; cleanup on unmount/dep change | `OnInitialized`/`OnDataChanged`/`OnUnmounted` stubs |
| `useRef(null)` | stable mutable ref; may point to a node | pointer/handle; no ownership |
| `useAnimatedValue(init)` | sampled animated value with tween/spring | `AnimationController` member |

- State updates are batched per tick/frame (a tick is one fixed-step update, [`game-loop.md`](../design/game-loop.md)); setters do not render synchronously.
- Effects run on the UI thread after commit; they must not block.
- An effect may declare a cleanup handler (IR field `cleanup`, [`ir-v1.md`](ir-v1.md) section 2; source form in [`jsx-subset-v1.md`](jsx-subset-v1.md) section 7). It runs before the effect runs again and on unmount.
- Effect dependency arrays are exact; V1 rejects missing/extra deps.
- Ref cleanup: refs are cleared when the target node is destroyed.
- Animated values are sampled at the fixed timestep (see [`animation.md`](../design/animation.md) and [`animation-v1.md`](animation-v1.md)).

## 3. Props and children

- Props are read-only; changing a prop triggers an update, not a state mutation.
- `children` is an explicit prop of type `Children`; the caller's child nodes become the `Component`
  node's `children` and the callee places them with `{children}` (a `Slot` node, [`ir-v1.md`](ir-v1.md)
  section 3.4).
- `key` is required for dynamic lists and stable across reorder.
- Spread props are allowed only when statically analyzable.
- Unknown props are compile errors for intrinsics; custom components validate their prop types.
- `className`, `style`, `id`, `role`, `aria-*`, and event props are reserved.

## 4. Events

- Supported V1 events: pointer down/up/move/enter/leave, click, key down/up, focus/blur,
  input/change, scroll, submit, pointer cancel.
- Event handlers are `bind::<id>` protected regions in generated C++ (a protected region is a developer-owned block that survives regeneration; [ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md), [`developer-workflow.md`](../guides/developer-workflow.md)).
- Handlers receive a normalized event object (`target`, `currentTarget`, position, key, etc.).
- Handlers may call state setters, request focus, or start animations; they must not block or
  mutate the scene graph directly.
- `stopPropagation`/`preventDefault` map to the scene/input dispatcher; `handled` is advisory.

## 5. Lifecycle

```text
create -> build -> mount -> (update | effect | animation)* -> unmount -> destroy
```

- **Build**: construct nodes/styles; no side effects.
- **Mount**: attach to the scene, run initial effects, request layout.
- **Update**: state/props change; reconcile nodes; run effects whose deps changed.
- **Unmount**: cancel effects/animations, release refs, detach nodes.
- **Destroy**: retire GPU resources after frames in flight.

Effects never run during render; they run after the commit boundary. Unmount cleanup is
deterministic and ordered child-first.

## 6. Reconciliation

- Component identity is stable per parent slot; keys preserve identity in lists.
- Updating props/style/text mutates nodes in place; it does not recreate the tree each frame.
- Insert/remove/reorder use keyed matching.
- Focus, selection, and scroll are preserved across updates when identity is stable.
- Dynamic lists without keys are compile errors; static lists may omit keys.
- Removed nodes are detached immediately and retired after the current frame.

## 7. Styling

- `className` references stylesheet classes; `style` is inline CSS.
- The style engine computes `ComputedStyle` (typed style after cascade, [`stylesheet-v1.md`](stylesheet-v1.md)) per node; components do not read computed values.
- Pseudo-states (`:hover`, `:active`, `:focus`, `:focus-visible`, `:disabled`, `:checked`) are
  driven by input and props.
- Theme tokens are custom properties on `:root`/theme scopes.
- Layout/paint/animation read the computed style, not JSX expressions.

## 7.1 Error boundaries

An error boundary wraps one subtree and shows a fallback after a caught error. It is a supported
V1 feature (see [`error-boundaries-v1.md`](error-boundaries-v1.md)):

```tsx
<ErrorBoundary fallback={<ErrorView />} onError={report} resetKey={routeId}>
  <Child />
</ErrorBoundary>
```

- Catches synchronous exceptions from child build/update/effect/event dispatch.
- Async errors are `Error` values reported to the owning boundary; they are not thrown.
- Nested boundaries: nearest catches; fallback/onError failure propagates to parent/root.
- Fatal/OOM/cancellation/platform errors are not caught by a component boundary.
- Reset via `resetKey`/manual retry; auto-retry is off by default and bounded.
- No silent catch: diagnostics + `onError`; redact secrets.

## 7.2 Custom native elements

A custom element is an intrinsic native-render node declared from JSX:

```tsx
<CustomElement
  element="Waterfall3D"
  style={{ width: 640, height: 360 }}
  props={{ intensity: 0.8, camera: "main" }}
  onPointer={handlePointer}
  onSelect={handleSelect}
/>
```

- `element` is a static literal declared in `ui/elements.json`; no dynamic component type.
- The C++ class derives `tessera::element::CustomElement` and uses the RHI/shader API.
- It is a leaf; use `OverlayHost`/overlay service ([`overlays-and-layers-v1.md`](overlays-and-layers-v1.md)) for overlays, and wrap in `View` for layout
  children.
- It participates in normal layout/style/automation/input and is reusable with keys/props/state.
- No JS runtime, dynamic plugin, runtime shader compilation, or raw backend/OS handles.
- See [`custom-elements-v1.md`](custom-elements-v1.md) and
  [`custom-elements.md`](../guides/custom-elements.md).

## 7.3 Text input and forms

- `TextInput`/`TextArea` are intrinsic leaf nodes with controlled/uncontrolled value,
  selection, IME/composition, undo/redo, clipboard, autofill, secure mode, and accessibility.
- Editing semantics are in [`text-editing-v1.md`](text-editing-v1.md).
- Form state/validation/submit/focus are in
  [`forms-and-validation-v1.md`](forms-and-validation-v1.md); `Form`/`Field` are library
  components, not new intrinsics.
- Text scale, bidi, grapheme clusters, and caret/selection layout follow the text stack ([ADR-006](../../decisions/ADR-006-text-stack.md) (text stack): FreeType, HarfBuzz, SheenBidi, ICU).
- Secure fields redact snapshots/logs and restrict clipboard/undo persistence.

## 8. Rejected in V1

- custom hooks, context, suspense, portals; use `OverlayHost`/overlay service instead
  ([`overlays-and-layers-v1.md`](overlays-and-layers-v1.md))
- dynamic component types (`<Component is={...}>`); `CustomElement` is an intrinsic node, not a
  dynamic component type
- arbitrary JavaScript expressions, closures, async render, generators
- direct DOM/window/OS access
- inline function components without a stable name
- runtime style object construction beyond static literals/allowlisted pure calls (the allowlist is not specified in this file)
- dynamic imports and code splitting

Rejections are compile errors with source spans; no silent fallback.

## 9. Generated C++ contract

- One `<Component>.g.h` + `<Component>.g.cpp` per component.
- State/effect/ref/animated members are generated with stable names.
- Node construction and updates are generated; developer logic is only inside protected
  regions (`bind::<id>`).
- Generated code is deterministic, formatted, and compiles with warnings-as-errors in CI.
- Node constructors carry `(NodeId, UiId, ComponentId)`; the automation manifest is emitted
  deterministically.
- `CustomElement` emits a generated factory registry + typed JSX declarations from
  `ui/elements.json`; no runtime registration.
- Error boundaries emit guards around child entry points; fallback/onError are outside the
  guard and propagate to the parent/root on failure.
- The binding manifest (generated JSON, see [ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md)) maps ids to components/files/regions.

## 10. Determinism

- Stable component/node/binding ordering.
- No timestamps, absolute paths, random ids, or wall clock.
- State/effect scheduling is deterministic for the same input sequence.
- Effects dependencies and update order are stable.
- Golden mode ([`golden-tests-v1.md`](golden-tests-v1.md)) injects the clock and input script.

## 11. Tests

- Hook rules: order, conditional calls, unsupported hooks, missing deps.
- State batching and setter behavior.
- Effect mount/update/unmount ordering and cleanup.
- Ref assignment/clear on mount/unmount.
- Keyed list reorder/insert/remove; focus/scroll preservation.
- Generated C++ compile tests and IR snapshots.
- Rejection fixtures for every unsupported construct.
- Custom elements: manifest/props/events, leaf rule, keyed reuse, lifecycle, input/snapshot,
  headless/golden tests.

## 12. Open questions

- Exact effect dependency analysis vs explicit user-provided arrays.
- `useMemo`/`useCallback`: decided, only the safe forms lower as plain locals/handlers; see
  [`react-compat.md`](jsx-intrinsics-v1/react-compat.md) section 4.2.
- Context-free dependency injection for services.
- Async effects/loading states without suspense.
- Error-boundary retry/reset and event-handler catch policy; see
  [`error-boundaries-v1.md`](error-boundaries-v1.md).
