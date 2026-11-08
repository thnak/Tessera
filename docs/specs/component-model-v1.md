# Component model v1

Status: proposed (Big Step C). Normative for the JSX/TSX subset and generated C++.

A Tessera component is a pure description of UI state and a node tree. It is compiled to C++;
there is no React runtime in production.

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

## 2. Hooks

| Hook | V1 semantics | Generated C++ |
|---|---|---|
| `useState(init)` | state + setter, batched updates | member + setter; marks dirty |
| `useEffect(fn, deps)` | run after commit when deps change; cleanup on unmount/dep change | `OnInitialized`/`OnDataChanged`/`OnUnmounted` stubs |
| `useRef(null)` | stable mutable ref; may point to a node | pointer/handle; no ownership |
| `useAnimatedValue(init)` | sampled animated value with tween/spring | `AnimationController` member |

- State updates are batched per tick/frame; setters do not render synchronously.
- Effects run on the UI thread after commit; they must not block.
- Effect dependency arrays are exact; V1 rejects missing/extra deps.
- Ref cleanup: refs are cleared when the target node is destroyed.
- Animated values are sampled at the fixed timestep (see `animation.md`).

## 3. Props and children

- Props are read-only; changing a prop triggers an update, not a state mutation.
- `children`/slots are explicit props; the compiler lowers them to child nodes.
- `key` is required for dynamic lists and stable across reorder.
- Spread props are allowed only when statically analyzable.
- Unknown props are compile errors for intrinsics; custom components validate their prop types.
- `className`, `style`, `id`, `role`, `aria-*`, and event props are reserved.

## 4. Events

- Supported V1 events: pointer down/up/move/enter/leave, click, key down/up, focus/blur,
  input/change, scroll, submit, pointer cancel.
- Event handlers are `bind::<id>` protected regions in generated C++.
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
- The style engine computes `ComputedStyle` per node; components do not read computed values.
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

## 8. Rejected in V1

- custom hooks, context, suspense, portals
- dynamic component types (`<Component is={...}>`)
- arbitrary JavaScript expressions, closures, async render, generators
- direct DOM/window/OS access
- inline function components without a stable name
- runtime style object construction beyond static literals/allowlisted pure calls
- dynamic imports and code splitting

Rejections are compile errors with source spans; no silent fallback.

## 9. Generated C++ contract

- One `<Component>.g.h` + `<Component>.g.cpp` per component.
- State/effect/ref/animated members are generated with stable names.
- Node construction and updates are generated; developer logic is only inside protected
  regions (`bind::<id>`).
- Generated code is deterministic, formatted, and compiles with warnings-as-errors in CI.
- Error boundaries emit guards around child entry points; fallback/onError are outside the
  guard and propagate to the parent/root on failure.
- The binding manifest maps ids to components/files/regions.

## 10. Determinism

- Stable component/node/binding ordering.
- No timestamps, absolute paths, random ids, or wall clock.
- State/effect scheduling is deterministic for the same input sequence.
- Effects dependencies and update order are stable.
- Golden mode injects the clock and input script.

## 11. Tests

- Hook rules: order, conditional calls, unsupported hooks, missing deps.
- State batching and setter behavior.
- Effect mount/update/unmount ordering and cleanup.
- Ref assignment/clear on mount/unmount.
- Keyed list reorder/insert/remove; focus/scroll preservation.
- Generated C++ compile tests and IR snapshots.
- Rejection fixtures for every unsupported construct.

## 12. Open questions

- Exact effect dependency analysis vs explicit user-provided arrays.
- Whether `useMemo`/`useCallback` are ever allowed (currently no).
- Context-free dependency injection for services.
- Async effects/loading states without suspense.
- Error-boundary retry/reset and event-handler catch policy; see
  [`error-boundaries-v1.md`](error-boundaries-v1.md).
