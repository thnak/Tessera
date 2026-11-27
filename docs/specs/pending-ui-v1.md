# Pending UI v1

Status: Proposed draft, waits for ADR-067 Acceptance. Nothing here is normative until
[ADR-067](../../decisions/ADR-067-pending-ui-transitions-suspense-lazy.md) is Accepted by the owner;
this draft follows the ADR and decides nothing it leaves open (section 12 lists those questions).
"V1" means the first-release scope ([AGENTS.md](../../AGENTS.md), Non-negotiables rule 8). Issue: #655.

Terms follow the ADR. A **resource** is the typed, keyed record of
[`async-data-v1.md`](async-data-v1.md) section 1. A **suspending read** is a read, declared
`suspend: true`, of a resource whose status is `Loading` with no data. A **boundary** is a `Suspense`
node. A **transition** is a state change whose UI effect may wait for pending resources while the old
UI stays visible. The **settled value** is the last value for which no transition read was pending.

## 1. Scope and what already exists

This spec defines six things: the `useResource` library hook (section 3), the `Suspense` IR node
(section 4), the transition mark and `isPending` (section 5), `useDeferredValue` (section 6), `lazy`
(section 7) and the diagnostics (section 9). It adds no concurrent renderer, no run-time component
object and no scheduler: the pending state is a fixed-size member of the generated component and an
update stays one synchronous, allocation-free pass ([`component-model-v1.md`](component-model-v1.md)).

State of the code at the time of writing, which this spec must not hide:

- The spike #661 built the boundary and the transition mark on a fake resource, off by default:
  `include/tessera/ui/pending/{suspense,transition,fake_resource}.hpp`,
  `src/compiler/eval/src/pending.cpp`, `src/compiler/codegen/src/emit_suspense.cpp`, and the tests of
  section 11. A component with neither a `Suspense` node nor a transition-marked action emits no
  pending code, so every committed `.g.hpp` golden is unchanged.
- The IR already has the `Suspense` kind, the optional `transition` flag on a `set` action and the
  validator rules for both. No `.tsx` syntax produces them: the fixture is hand-built IR.
- Neither `useResource`, `useTransition`, `useDeferredValue` nor `lazy` is recognized by the front end
  ([`behavior-hooks-v1.md`](behavior-hooks-v1.md) section 1 describes the same gap and the generic
  library-hook path this spec reuses). `useService` is in the IR and codegen but not lowered from TSX.
- The data store (`src/runtime/data_store*`) is not reachable from a generated component and lacks four
  things (section 3.3).
- `useDeferredValue` and `lazy` have no spike; their rules here follow the ADR text only.

## 2. The rule in one place

1. A boundary shows its **fallback** while any suspending read it covers is `Loading` with no data, and
   its **subtree** otherwise (ADR-067 item 2).
2. `Refreshing` never suspends: a resource that has data keeps showing it while a new fetch runs
   (item 3).
3. A transition keeps the **old** subtree and reports `isPending` while a read it started is
   `Loading` or `Refreshing` (item 4).
4. A completion for a superseded epoch is dropped, never applied ([`async-data-v1.md`](async-data-v1.md)
   section 6).
5. Nothing fetches in render: a hook subscribes and reads, a fetch starts only from a key change or an
   explicit action.

## 3. `useResource`

### 3.1 Call form and manifest entry

```tsx
const feed = useResource(api.loadFeed, { query }, { suspend: true });
// feed.status, feed.data, feed.error
```

`useResource(service.member, key, options?)` is a library hook of the built-in hook manifest
([`jsx-subset-v1.md`](jsx-subset-v1.md) section 7.1), manifest kind `environment`, per instance
(ADR-067 item 1). The first argument is a service method (`useService`, ADR-050), never a URL, so
networking stays in services. The second is the key, a typed pure expression of state, props or
environment values. `options` is one object literal; its only V1 field is `suspend` (boolean, default
`false`).

Rules:

- The result is a read-only record `{ status, data, error }` with the types of
  [`async-data-v1.md`](async-data-v1.md) section 1 (`status` is a `DataStatus`, `data` and `error` are
  optional). Each field is readable in pure expressions, JSX and effect dependencies, as an
  `environment` value is.
- The hook subscribes and starts a fetch for the key through the data store. A key change starts a new
  epoch; the previous data, if any, is kept, so the status becomes `Refreshing`, not `Loading`.
- It MUST NOT fetch, allocate or block during render. A call outside a component body, in a loop or in a
  condition is the existing hook-order error of the core hooks.
- A resource declared `suspend: true` is a **suspending read** and MUST be covered by a boundary
  (section 9). A resource without `suspend` never suspends; the component reads `status` itself.
- Reading `data` while it is absent yields the IR zero value of its type; code that needs the
  distinction reads `status`.

### 3.2 What it reads from the store

`useResource` reads four facts per key, all of which the store already keeps
([`async-data-v1.md`](async-data-v1.md) sections 1, 6 and 8.1):

| Fact | Source | Used for |
|---|---|---|
| `status` | `Resource<T>::status` | the status field, `suspends()`, `refreshing()` |
| `data` / `error` | `Resource<T>` | the record fields |
| `epoch` | `Resource<T>::epoch`, bumped on every key, parameter or cancel change | dropping a stale completion; the transition's own epoch (section 5) |
| fetch in flight | `Loading` or `Refreshing` | `isPending` |

### 3.3 What the data store lacks

The spike recorded four gaps (ADR-067, spike evidence). Each is a prerequisite of an implementation
issue, not something this spec papers over:

1. `DataStatus::Stale` is declared and never produced. `useResource` MUST read `epoch`, not `Stale`.
   Whether to remove the enumerator or define when it is produced is open (section 12, question 1).
2. There is no `suspends()`/`refreshing()` pair. The store MUST expose them (or `useResource` computes
   them in one place): `suspends() == (status == Loading && !has_data)`,
   `refreshing() == (status == Refreshing)`. The spike's `SuspenseRead` is the model.
3. There is no allocation-free synchronous read. The store MUST offer a "what is published right now"
   accessor that does not allocate or take a lock the UI thread can wait on; the boundary and the
   hook read through it. Completion still arrives through the `CompletionQueue` at a tick boundary.
4. The store is not reachable from generated components. It is reached as a service
   (`useService`, ADR-050); until the front end lowers `useService`, nothing in this spec can run
   from TSX.

### 3.4 IR

`useResource` lowers to one entry of an optional component list `resources` (additive, omitted when
empty, so a component without resources writes the bytes it wrote before):

```json
{ "id": "res::feed", "service": "svc::api", "method": "loadFeed", "suspend": true,
  "key": { "kind": "record", "fields": { "query": { "kind": "ref", "id": "state::query" } } },
  "source": { "file": "ui/Feed.tsx", "offset": 120, "length": 60, "line": 6, "column": 3,
              "endLine": 6, "endColumn": 63 } }
```

- Reference ids `res::<name>.status`, `res::<name>.data` and `res::<name>.error` resolve in
  expressions; `<name>` is the local binding name, or `res::<method>_<n>` for a bare call, counted from
  1 (the rule of `beh::` in [`behavior-hooks-v1.md`](behavior-hooks-v1.md) section 5).
- `service` MUST name a declared `svc::` entry; `key` MUST type-check. Entries keep declaration order
  (determinism, [`ir-v1-expr.md`](ir-v1-expr.md) section 11). The exact JSON field names are provisional
  until the ADR is Accepted.

## 4. The `Suspense` IR node

### 4.1 Shape

`Suspense` has the shape of `ErrorBoundary` ([`error-boundaries-v1.md`](error-boundaries-v1.md)
section 3): one `fallback`, one child subtree, no scene node and no layout box.

```tsx
<Suspense fallback={<Spinner />}>
  <FeedList items={feed.data} />
</Suspense>
```

```json
{
  "id": "node::s",
  "kind": "Suspense",
  "fallback": { "kind": "View", "id": "node::fb", "children": [] },
  "children": [
    { "kind": "View", "id": "node::list", "children": [] }
  ]
}
```

Rules:

- `fallback` is required and is one static element subtree: not conditional, not looped. The validator
  already applies the `ErrorBoundary` rule to both kinds (`TESSERA_IR_ERROR_BOUNDARY`).
- `children` is exactly one subtree; wrap siblings in a `View`.
- The boundary has no `props` and no `events` in V1: no `resetKey`, no `onError`, no timeout.
- Nested boundaries are allowed; a read is covered by the **nearest** enclosing boundary.
- Only `ErrorBoundary` and `Suspense` may carry `fallback` (`TESSERA_IR_MALFORMED` otherwise).
- The boundary is not painted and does not affect layout.

### 4.2 Which reads a boundary covers

A boundary covers every suspending read declared in its child subtree, including reads of component
instances inlined or nested inside it. A suspending read outside any boundary of its component tree is
`TESSERA_SUSPENSE_NO_BOUNDARY`. How a read is attached to a boundary across a `Component` node is
open (section 12, question 3).

### 4.3 Behavior

| Phase | When | Live subtree |
|---|---|---|
| Content | no covered read suspends | the child subtree |
| Fallback | some covered read suspends | the fallback subtree |

- The subtree's **state is kept** while the fallback shows (ADR-067 item 2): it is detached, not
  destroyed, and returns without a rebuild.
- A boundary tracks at most eight reads (`SuspenseBoundary::kMaxReads` in the spike). More than eight in
  one boundary is open (section 12, question 6).
- A read slot that was never registered is a recoverable error `TESSERA_SUSPENSE_UNKNOWN_READ`, never a
  silent drop: ignoring it would show content while loading.
- The swap moves ownership between two slots and re-parents the scene child; it MUST NOT allocate after
  warm-up (section 11).
- The fallback keeps the layout stable and announces loading once, at the bounded rate of
  [`accessibility-v1.md`](accessibility-v1.md) section 10.1 (identical messages within 250 ms).

### 4.4 Versioning

The change is additive; `irVersion` stays `1` ([`ir-v1-expr.md`](ir-v1-expr.md) section 12):

- [`ir-v1.md`](ir-v1.md) section 3: `Suspense` joins the node `kind` list (the table row for `kind`
  next to `ErrorBoundary`). The code already has it (`NodeKind::Suspense`).
- [`ir-v1-expr.md`](ir-v1-expr.md): a new section 5.2 `Suspense`, after 5.1 `ErrorBoundary`, and the
  section 12 line that lists additive kinds.
- [`ir-v1.md`](ir-v1.md) section 2 (and the Component table): the optional `resources` list of
  section 3.4, with the `res::` reference ids added to the reference id list.
- [`versioning-v1.md`](versioning-v1.md): no new row (the schema version is unchanged).

An `irVersion` 1 consumer that does not know `Suspense` or `resources` rejects such a module
(`TESSERA_IR_MALFORMED`); the compiler, evaluator and generator ship together, the accepted risk of
`types`/`record`. This draft does not edit either IR spec; that is part of the implementing change.

## 5. The transition mark and `isPending`

```tsx
const [isPending, startTransition] = useTransition();
<Button onClick={startTransition(() => setQuery(next))}>Search</Button>
{isPending ? <Text>Updating</Text> : null}
```

- `useTransition()` returns `[isPending, startTransition]` (ADR-067 item 4). `isPending` is a
  **behavior output** ([`behavior-hooks-v1.md`](behavior-hooks-v1.md) section 5): a read-only bool with
  reference id `beh::<name>.isPending` (default `false` before the first transition), and a change marks
  the component updated, at most one update per input dispatch.
- `startTransition` takes exactly one **setter-call arrow**
  ([`jsx-subset-v1.md`](jsx-subset-v1.md) section 9.2). It lowers to the same `set` actions the arrow
  would lower to, plus `"transition": true` on each:

```json
{ "id": "handler::search",
  "actions": [ { "kind": "set", "target": "state::query",
                 "value": { "kind": "ref", "id": "prop::next" }, "transition": true } ] }
```

- The flag is written only when true, so a module without transitions is byte-identical to before.
  The validator MUST reject a mark on anything but a state setter (`TESSERA_IR_TRANSITION`, implemented).
- Applying the marked setter calls `TransitionMark::begin(epoch)`, with the epoch of the resource the
  change writes. While a read started by that change is `Loading` or `Refreshing`, `isPending` is true
  and a boundary above it keeps the **old** subtree instead of its fallback.
- The mark holds one epoch and one flag. `observe(loading, epoch)` ignores a completion whose epoch is
  not the mark's own, so a late response for a superseded key cannot clear a newer transition.
- When the read settles (data, or failure keeping the old data), `isPending` becomes false.
- An `isPending` UI shows a subtle indicator and never clears the screen
  ([`async-data-v1.md`](async-data-v1.md) section 6).
- `startTransition` is never aliased to "never pending" (ADR-067, alternatives).

## 6. `useDeferredValue`

- `useDeferredValue(value)` returns the **settled value** (ADR-067 item 5): the last `value` for which no
  suspending read of the transition lane was pending, and `value` itself when nothing is pending.
- It is a behavior output of the same engine class as the transition, so the result is read-only,
  typed as `value`, updates the component like any output, and is allocation-free (one stored value).
- Its meaning is weaker than React's: it is the settled value, not a lower-priority render. Ported code
  gets the same result but not the same timing, and the documentation MUST say so.
- It has no spike. Its storage for non-scalar types, and what "the transition lane" is when no
  transition is active, are open (section 12, questions 4 and 5).

## 7. `lazy` as identity

- `const Heavy = lazy(() => import("./Heavy"))` resolves the import at compile time and renders
  `Heavy` immediately (ADR-067 item 6). A native app is one binary and fetches no code.
- The call form is exactly that: one arrow with one `import(...)` of a project-relative specifier that
  resolves to a component. Anything else is the existing `TESSERA_REACT_UNSUPPORTED_API`.
- A `lazy` component is valid inside `Suspense` and never suspends by itself. The documentation MUST
  state that no splitting occurs. A web bundle split (ADR-046) may later back it without a source change.
- It produces no IR of its own: lowering emits the `Component` node of the resolved import.

## 8. Accessibility and layout

One rule (ADR-067 item 7): a fallback keeps the layout stable and announces loading once, bounded by
[`accessibility-v1.md`](accessibility-v1.md) section 10.1; an `isPending` UI shows a subtle indicator
and never clears the screen. Returning from a fallback restores focus to the control that held it when
the subtree is still mounted (state kept, section 4.3).

## 9. Diagnostics

All are errors unless noted. Where a code is registered depends on the stage:
analyzer codes (`TESSERA_REACT_*`, `TESSERA_SUSPENSE_*` raised from TSX) are registered in
`src/compiler/analyzer/include/tessera/compiler/jsx/diagnostic_registry.hpp` with a fixture and a line in
`tests/golden/jsx/diagnostics-registry.txt`, in the same change, as
[`behavior-hooks-v1.md`](behavior-hooks-v1.md) section 7 requires; IR codes (`TESSERA_IR_*`) are raised
by the validator ([`ir-v1-expr.md`](ir-v1-expr.md) section 9); recoverable run-time codes are literals of
the pending model.

| Code | Stage | When |
|---|---|---|
| `TESSERA_SUSPENSE_NO_BOUNDARY` | analyzer, and validator on hand-built IR | A `useResource` declared `suspend: true` has no `Suspense` ancestor (ADR-067 item 2). Never a blank screen |
| `TESSERA_IR_ERROR_BOUNDARY` | validator (exists) | A `Suspense` has no `fallback`, or its fallback is conditional or looped |
| `TESSERA_IR_TRANSITION` | validator (exists) | A `transition` mark on an action that is not a state setter |
| `TESSERA_IR_MALFORMED` | validator (exists) | `fallback` on a kind other than `ErrorBoundary`/`Suspense` |
| `TESSERA_SUSPENSE_UNKNOWN_READ` | run time (exists, recoverable `PendingError`) | A read slot was never registered on the boundary |
| `TESSERA_REACT_HOOK_ARG` | analyzer (exists) | `useResource` arguments are not of the declared kind or type, or a key is not pure |

The ADR lists only `TESSERA_SUSPENSE_NO_BOUNDARY`; this draft adds no other new analyzer code.
Diagnostics for a `startTransition` argument that is not a setter-call arrow and for an unresolvable
`lazy` import reuse the existing handler and import codes; whether they need their own is open
(section 12, question 7).

## 10. Generated code and the evaluator

- The generated base owns both subtrees of each boundary as ownership slots, a live pointer, and the
  parent and index where the live subtree belongs; the swap calls `insert_child`/`remove_child` on the
  scene's existing child storage. Members are named `suspense_<n>_*`; the boundary, the transition mark
  and the resource are fixed-size members.
- A boundary or transition-marked action is the only trigger for emitting pending code; a component with
  neither emits none and stays byte-identical to its golden.
- `tessera::ui::pending` is header-only so the evaluator (links only `tessera_ir`) and generated C++
  (links `tessera_scene`) run **one** state machine; the differential test therefore proves the wiring.
- The preview MUST show the fallback and the pending state, with resources faked by test doubles or the
  Studio mock worker (ADR-055). Not implemented in the spike.
- Generated code MUST NOT fetch in render, MUST NOT wait on the UI thread, and MUST NOT throw across
  threads or FFI (no promise is thrown; ADR-067 alternatives).

## 11. Verification layers

Every normative rule has a test at one of three layers, plus the usual front-end and IR layers.

| Layer | Proves | Where (spike) |
|---|---|---|
| Model | fallback, restore, keep-on-refresh, `isPending`, stale-epoch drop | `pending_model` (`tests/unit/pending_model_tests.cpp`) |
| Evaluator vs generated C++ | one fixed 20-step script gives identical observable state after every step | `codegen_pending_parity`, fixture `tests/unit/codegen_pending_module.hpp` |
| Generated component | the compiled `pending::Feed` behaves as the model and compiles on MSVC and GCC | `codegen_pending_runtime` |
| Codegen text | the generated base and the golden `tests/golden/codegen/pending/Feed.g.hpp` | `codegen_pending` |
| Allocation-free update | after a three-cycle warm-up, 64 transition cycles allocate zero (global `operator new` counter) | `codegen_pending_runtime` |
| Determinism with a fake resource | the fake resource (`include/tessera/ui/pending/fake_resource.hpp`) has the store's status and epoch rules, no clock, no randomness | all of the above |

The differential script is: idle, fetch with no data, restore, refresh, two fetches where the first is
superseded, a marked setter, the settle, a failed refresh; compared fields are phase, live subtree,
`isPending`, epoch, value, loading and has-data.

### 11.1 Conformance checklist

An implementation conforms when all of these hold (new tests are named in the implementing issues):

- [ ] A suspending read with no data shows the fallback; the data restores the subtree with state kept.
- [ ] A key change on a resource with data is `Refreshing` and keeps the subtree (no fallback).
- [ ] `isPending` is true from the marked setter until the fetch settles, for `Loading` and `Refreshing`.
- [ ] A completion for a superseded epoch is dropped and cannot clear a newer transition.
- [ ] A failed refresh keeps the old data and clears `isPending`.
- [ ] A `suspend: true` read with no boundary is `TESSERA_SUSPENSE_NO_BOUNDARY`.
- [ ] `Suspense` without a fallback, or with a conditional fallback, is rejected by the validator.
- [ ] A transition mark on a non-setter action is `TESSERA_IR_TRANSITION`.
- [ ] Nested boundaries: the nearest covers the read.
- [ ] `useDeferredValue` equals its argument when nothing pends, and the last settled value otherwise.
- [ ] `lazy` renders the component at once, inside and outside `Suspense`.
- [ ] The evaluator and generated code agree on the differential script (all compilers of the CI matrix).
- [ ] Updates allocate nothing after warm-up; the data-store read is allocation-free (section 3.3).
- [ ] A component without `Suspense`, resources or transition marks is byte-identical in IR and in
      generated code to its pre-spec golden.
- [ ] The preview shows the fallback and the pending state with a faked resource.
- [ ] The fallback announces loading at most once per 250 ms window (accessibility).

## 12. Open questions

Where ADR-067 is silent or ambiguous; none is decided here.

1. **`DataStatus::Stale`.** Remove it, or define when the store produces it. This spec reads `epoch`
   either way (the ADR's spike note calls this the one decision needed before the spec).
2. **`useResource` IR shape.** A new `resources` list as drafted in section 3.4, or reuse of the
   `environment` list with a provider. The ADR says "manifest kind `environment`" but resources have a
   key, a method and an epoch that a plain environment value lacks.
3. **Coverage across components.** The ADR says "any descendant"; whether a read inside a nested
   `Component` node (not inlined) is covered by the parent's boundary, and how codegen wires that read
   to the boundary's slot, is not stated. The spike has one resource per component.
4. **The transition lane.** `useDeferredValue` is defined by "the transition lane"; the ADR does not say
   whether `useDeferredValue` alone (no `useTransition` in the component) can ever be pending, or which
   reads belong to it.
5. **Deferred value storage.** Scalars are clear; strings and records need a copy, which conflicts with
   allocation-free updates unless bounded.
6. **Read limit.** The spike caps a boundary at eight reads; whether that is a language limit with a
   diagnostic or an implementation detail is unspecified.
7. **Extra diagnostics.** Whether a non-setter `startTransition` argument, a `useResource` outside a
   service-capable component, or an unresolvable `lazy` import warrants a dedicated code.
8. **Key identity.** How a key (a record) is compared for "key change" and hashed for the store.
9. **Interaction with `ErrorBoundary`.** An error resource: the ADR keeps `Error` as a status value
   shown through `status`/`error`; whether it can also be routed to the nearest `ErrorBoundary` is open
   ([`error-boundaries-v1.md`](error-boundaries-v1.md) section 10 already lists the interaction).

## 13. Related documents

- [ADR-067](../../decisions/ADR-067-pending-ui-transitions-suspense-lazy.md) (the decision and the spike evidence).
- [`async-data-v1.md`](async-data-v1.md) (resources, epochs, UI integration);
  [`error-boundaries-v1.md`](error-boundaries-v1.md) (the boundary precedent);
  [`behavior-hooks-v1.md`](behavior-hooks-v1.md) (output rules, manifest path, per-hook spec and
  verification map conventions, issues #638, #639, #641);
  [`ir-v1.md`](ir-v1.md) and [`ir-v1-expr.md`](ir-v1-expr.md) (node and versioning conventions);
  [`component-model-v1.md`](component-model-v1.md) (update model); [`accessibility-v1.md`](accessibility-v1.md).
- [`../reference/react-core-coverage.md`](../reference/react-core-coverage.md) rows for
  `useTransition`, `useDeferredValue`, `lazy` and `Suspense` change when the ADR is Accepted.
