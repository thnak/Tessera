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

## Evidence required before Accepted

This ADR stays Proposed until measured evidence exists. Spike [#661](https://github.com/thnak/Tessera/issues/661) implements a Suspense boundary and a transition mark on a fake resource, off by default, and records: fallback, restore, keep-on-refresh, pending flag and stale-epoch behavior; agreement between the evaluator and generated code; allocation-free updates; and what the data store lacks. The implementing issues #655 to #657 wait for Acceptance.

### Spike #661 evidence (items 1-4, recorded 2026-11-21)

Built off by default: `include/tessera/ui/pending/{suspense,transition,fake_resource}.hpp`,
`src/compiler/eval/{include/tessera/compiler/eval/pending.hpp,src/pending.cpp}`,
`src/compiler/codegen/src/emit_suspense.{hpp,cpp}`, tests `pending_model`, `codegen_pending`,
`codegen_pending_runtime`, `codegen_pending_parity`, fixture
`tests/unit/codegen_pending_module.hpp` + `tests/unit/codegen_base/pending/Feed.{hpp,cpp}`, golden
`tests/golden/codegen/pending/Feed.g.hpp`. Nothing runs in a shipped app: a component with neither a
`Suspense` node nor a transition-marked action emits no pending code at all, which is why every
committed `.g.hpp` golden is byte-identical.

**The model holds.** The boundary is a static node of the `ErrorBoundary` shape: one `fallback`, one
child subtree, no scene node and no layout box. It shows the fallback while a read it covers is
`Loading` with no data, restores the subtree when the data arrives, and keeps the old subtree while
the read is `Refreshing` — so a key change never blanks the screen. `isPending` is true from the
moment a marked setter is applied until the change's fetch settles, and a completion for a superseded
epoch is dropped rather than applied. All five behaviours are asserted on the model
(`pending_model`), on the generated component (`codegen_pending_runtime`) and across both
(`codegen_pending_parity`).

**The evaluator and the generated code agree.** `codegen_pending_parity` plays one fixed 20-step
script — idle, fetch-with-no-data, restore, refresh, two fetches where the first is superseded, a
marked setter, the settle, a failed refresh — on `eval::PendingSession` and on the compiled
`pending::Feed`, and compares the whole observable state (phase, live subtree, `isPending`, epoch,
value, loading, has-data) after every step. The two sides are not two implementations of the rule:
`tessera::ui::pending` is header-only precisely so the evaluator, which links only `tessera_ir`, and
generated C++, which links `tessera_scene`, run the same state machine. The differential test
therefore proves the *wiring* — that both are driven by the same operations in the same order — not
that two copies of the arithmetic happen to match.

**The update stays allocation-free.** `codegen_pending_runtime` replaces global `operator new` and
counts, the same gate the virtual pool uses. After a three-cycle warm-up, 64 full transition cycles
(fetch, sync, resolve, sync, plus both accessors) perform **zero** allocations. The boundary, the
mark and the fake resource are fixed-size members; the swap moves `unique_ptr`s between ownership
slots and calls `insert_child`/`remove_child`, which reuse the scene's existing child storage. The
warm-up is honest about the one exception: the first swap may grow a child vector.

**What the data store was missing, and what `useResource` needs from it.** The store
(`docs/specs/async-data-v1.md`) already has the two facts the boundary needs per key — "a fetch is in
flight" and "the epoch a completion must still match" — and `DataStoreCore::complete_fetch` already
drops a stale epoch. Four gaps, none of them blocking, all recorded rather than papered over:

1. **`DataStatus::Stale` is declared and never produced.** The enum has six members; nothing in
   `src/runtime/data_store*` ever assigns `Stale`. The spike's staleness is the per-key `epoch`
   counter, which is what actually works, so `useResource` should read `epoch` and not `Stale` — or
   the status should be removed. This is the one item that needs a decision before the spec is
   written.
2. **No "suspends" predicate.** `Loading` and `Refreshing` are both "a fetch is in flight"; only
   `Loading` with no previous value suspends. The store exposes the status but not the pair, so every
   consumer re-derives `status == Loading && !has_value`. `useResource` should expose
   `suspends()`/`refreshing()` as the boundary's `SuspenseRead` does.
3. **No synchronous read for a render.** The store notifies through `CompletionQueue` at a tick
   boundary and hands out `FetchHandle`s; there is no allocation-free "what is published right now"
   accessor. The spike's `sync_read` is exactly that call, and `useResource` needs it: a boundary
   must never fetch in render, only subscribe and read.
4. **The store is not reachable from generated components.** It is a service, and `useService` is not
   lowered by the front end yet (this ADR's own risk note). The spike therefore drives a fake
   resource with the store's contract in miniature rather than the store itself.

**What this does not settle.** The front end is untouched: neither `useResource` nor `useTransition`
is lowered from TSX, so the fixture is hand-built IR, exactly as the ADR's risk note predicts. The
`Suspense` node is in the IR and the validator, but no `.tsx` syntax produces it and
`docs/specs/pending-ui-v1.md` does not exist yet. `useDeferredValue` and `lazy` (items 5 and 6) have
no spike. The preview does not show the fallback or the pending state. And the GCC `-Werror` lane was
**not run** for this spike: the remote Linux box was unreachable, so the evidence above is the
Windows `dev` preset only (MSVC `/W4 /permissive- /WX`). The GCC lane must be run before Acceptance.

## Follow-up

- `docs/specs/pending-ui-v1.md`: the manifest entries, the `Suspense` IR node, the transition mark on
  handler actions, the diagnostics, the verification layers.
- The front end for `useService` and `useResource` (the same library-hook path as
  [`behavior-hooks-v1.md`](../docs/specs/behavior-hooks-v1.md) section 1).
- Evaluator and preview support: the preview must show the fallback and the pending state, with
  resources faked through test doubles or the Studio mock worker (ADR-055).
- Update `react-core-coverage.md` when accepted.
