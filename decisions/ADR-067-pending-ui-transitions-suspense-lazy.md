# ADR-067: Pending UI: `useResource`, `Suspense`, transitions, deferred values and `lazy`

- Status: Proposed (2026-11-20). Owner direction: the "L" rows of
  [`react-core-coverage.md`](../docs/reference/react-core-coverage.md) are done early, because the
  farm protocol of `AGENTS.md` can exhaust the roadmap sooner than planned and a larger codebase is
  harder to change. The model below is the proposal to review; it builds on what exists and adds no
  concurrent renderer.
- Date: 2026-11-20
- Related: [`async-data-v1.md`](../docs/specs/async-data-v1.md) (the `Resource<T>` model, implemented
  in `src/runtime/data_store*.cpp`), [ADR-021](ADR-021-async-runtime.md) (async runtime),
  [ADR-050](ADR-050/README.md) (services, `useObservable`),
  [`error-boundaries-v1.md`](../docs/specs/error-boundaries-v1.md) (`ErrorBoundary`, the precedent for a
  static boundary node), [ADR-065](ADR-065-custom-hooks-by-compile-time-inlining.md),
  [ADR-064](ADR-064-astryx-style-component-system.md), [ADR-036](ADR-036-own-jsx-frontend-and-react-like-model.md).
- Terms: a **resource** is the typed, keyed data record of `async-data-v1.md` with a status (`Idle`,
  `Loading`, `Refreshing`, `Success`, `Error`, `Stale`). A **suspending read** is a read of a resource
  that has no data yet. A **boundary** is a `Suspense` element. A **transition** is a state change
  whose UI effect is allowed to wait for pending resources while the old UI stays visible.

## Context

React's `Suspense`, `useTransition`, `useDeferredValue` and `lazy` let a screen wait for data without
blanking. Tessera has no concurrent renderer and does not want one: a component compiles to generated
code with fixed state slots, and an update is one synchronous pass (`component-model-v1.md`). But the
**ingredients** exist: the data store keeps a resource per key, a key change starts a fetch under a new
epoch, `Refreshing` keeps the previous data, and a late response for an old epoch is discarded
(`async-data-v1.md` sections 1 and 6). The UI side, how a component reads a resource and what a
screen does while it loads, is not specified, so applications would each invent it, and a ported React
screen that uses `Suspense` has no target.

Deciding the model before the codebase grows lets every later component (Table with server data,
Selector with a search, dialogs with a lazy body) be written against one rule.

## Decision

1. **`useResource` reads a resource from a service.** `useResource(svc.member, key)` is a library hook
   (manifest kind `environment`, per-instance) that returns a read-only record of the resource:
   `{ status, data, error }`. It registers a fetch for `key` through the data store; a key change starts a
   new epoch. No fetch happens in render: the hook only subscribes (`async-data-v1.md` section 6). A
   service method, not a URL, names the source, so networking stays in services (ADR-050).
2. **`Suspense` is a static boundary node.** `<Suspense fallback={<Spinner/>}>...</Suspense>` is an
   intrinsic of the same shape as `ErrorBoundary`: one `fallback`, one child subtree, no run-time
   component. While any descendant has a **suspending read** (a `useResource` declared with
   `suspend: true`, whose status is `Loading` with no data), the boundary renders the fallback in place
   of the subtree and keeps the subtree's state; when every read has data, the subtree returns. A
   component that suspends with no `Suspense` ancestor is `TESSERA_SUSPENSE_NO_BOUNDARY` (an error), so
   a missing boundary is never a blank screen.
3. **`Refreshing` does not suspend.** A key change on a resource that already has data moves it to
   `Refreshing` with the old data; the boundary keeps showing the subtree. This is the behavior React
   needs a transition for, and here it is the default of the data store.
4. **`useTransition` marks a state change as a transition.** It returns `[isPending, startTransition]`;
   `startTransition(() => setQuery(q))` takes a setter-call arrow (`jsx-subset-v1.md` section 9.2) and
   lowers to the same actions plus a transition mark. While a resource started by that change is
   `Loading` or `Refreshing`, `isPending` is `true` and a boundary above it keeps the **old** subtree
   instead of showing its fallback. `isPending` is a behavior output (ADR-064 and
   [`behavior-hooks-v1.md`](../docs/specs/behavior-hooks-v1.md)), so it follows the output rules.
5. **`useDeferredValue(value)` returns the settled value.** It is the last `value` for which no
   suspending read of the transition lane was pending; it equals `value` when nothing is pending. It is
   a behavior output of the same engine class as the transition, so a search box can show the typed text
   at once and the filtered list from the settled text.
6. **`lazy` is accepted as an identity.** `const Heavy = lazy(() => import("./Heavy"))` resolves the
   import at compile time and renders `Heavy` immediately. A native app is one binary and has no code
   to fetch; on the web target (ADR-046) a bundle split may later back it without a source change. A
   `lazy` component is still valid inside `Suspense`.
7. **One rule for accessibility and layout.** A fallback keeps the layout stable and announces loading
   once, at the bounded rate of `accessibility-v1.md` section 10.1; an `isPending` UI shows a subtle
   indicator and never clears the screen (`async-data-v1.md` section 6).
8. **Server data in components follows from this.** The Table plugins of ADR-064 item 8 pass a request
   token to `onLoadMore`; with this ADR the same token is the epoch of the resource, so a stale page is
   dropped by one mechanism.

## Consequences

### Positive

- A ported screen that uses `Suspense`, `useTransition` or `lazy` has a target, and the semantics follow
  the data store the engine already has instead of a second scheduler.
- One boundary rule for loading, the same shape as `ErrorBoundary`, statically checked.
- No new renderer, no run-time component objects, no allocation on update.
- Library components with data (Table, Selector, Typeahead) are specified against one model.

### Negative / risks

- It is a new IR node (`Suspense`), a new library hook (`useResource`) and two behaviors; each needs its
  spec and tests.
- `useDeferredValue` has a weaker meaning than React's (it is the settled value, not a lower-priority
  render): code that relies on React's rendering priority gets the same result but not the same timing.
- `lazy` as identity hides that no splitting occurs; the documentation must say so.
- The model depends on the data store being reachable from generated components through services
  (ADR-050); until `useService` is lowered by the front end, nothing here can run.

## Alternatives considered

- **A concurrent renderer with priorities.** Rejected: it would replace the fixed-slot, allocation-free
  update model that the engine is built on, for a gain the data store's epochs already give.
- **`Suspense` as a run-time component that catches thrown promises.** Rejected: exceptions across
  threads and FFI are forbidden, and there is no promise to throw.
- **Aliasing `useTransition` to "never pending".** Rejected: it would compile ported code and silently
  remove the pending UI.
- **No `lazy`.** Rejected: ported code uses it for routes; an identity costs a parser rule.

## Follow-up

- `docs/specs/pending-ui-v1.md`: the manifest entries, the `Suspense` IR node, the transition mark on
  handler actions, the diagnostics, the verification layers.
- The front end for `useService` and `useResource` (the same library-hook path as
  [`behavior-hooks-v1.md`](../docs/specs/behavior-hooks-v1.md) section 1).
- Evaluator and preview support: the preview must show the fallback and the pending state, with
  resources faked through test doubles or the Studio mock worker (ADR-055).
- Update `react-core-coverage.md` when accepted.
