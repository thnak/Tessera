# Pending UI v1

Status: Accepted with [ADR-067](../../decisions/ADR-067-pending-ui-transitions-suspense-lazy.md)
(owner, 2026-11-28); normative. The nine questions the draft left open are decided in section 12 and
folded into the sections they touch. "V1" means the first-release scope
([AGENTS.md](../../AGENTS.md), Non-negotiables rule 8). Issues: #655 (this spec), #656
(implementation).

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
  validator rules for both.
- Implemented (#656, slices 1 to 3): `<Suspense fallback={...}>` lowers to the `Suspense` node (sections
  4, 9), `lazy(() => import(...))` is identity (section 7), `useTransition()` lowers to a
  transition-marked `set` action plus the read-only `isPending` (section 5), `useResource` lowers to
  the `resources` list with typed `.data` and `.status` (section 3), `useDeferredValue` lowers to a
  `deferred_value` behavior (section 6), the boundary holds its old subtree while a transition is
  pending (section 4.3), and the evaluator and the generated base run all of it; `TESSERA_SUSPENSE_NO_BOUNDARY`
  and `TESSERA_SUSPENSE_TOO_MANY_READS` surface from TSX with source positions (section 9). The tests are
  `lowering_pending`, `lowering_resource`, `codegen_pending_tsx` and the model tests of section 11.
  Still open: `.error` has no generated form, the data-store adapter publishes no typed `.data`, a
  resource in an effect dependency list is not accepted, and the Studio preview does not show the
  pending state yet (#657).
- The data store (`src/runtime/data_store*`) is reached through a `ResourceView` the host binds and lacked
  four things (section 3.3), now provided.
- `useDeferredValue` has no spike; its rules here follow the ADR text and the decisions of section 12.

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
environment values whose type is a scalar or a record of scalars (decision 8). `options` is one object literal; its only V1 field is `suspend` (boolean, default
`false`).

Rules:

- The result is a read-only record `{ status, data, error }` with the types of
  [`async-data-v1.md`](async-data-v1.md) section 1 (`status` is a `DataStatus`, `data` and `error` are
  optional). Each field is readable in pure expressions, JSX and effect dependencies, as an
  `environment` value is.
- The hook subscribes and starts a fetch for the key through the data store. A key change starts a new
  epoch; the previous data, if any, is kept, so the status becomes `Refreshing`, not `Loading`.
  Two keys are equal when their values are equal, field by field; the store hashes the canonical
  encoding of the key (scalars in declaration order of the record, no pointers, no padding), so equal
  keys always share one resource entry.
- It MUST NOT fetch, allocate or block during render. A call outside a component body, in a loop or in a
  condition is the existing hook-order error of the core hooks.
- A resource declared `suspend: true` is a **suspending read** and MUST be covered by a boundary
  (section 9). A resource without `suspend` never suspends; the component reads `status` itself.
- Reading `data` while it is absent yields the IR zero value of its type; code that needs the
  distinction reads `status`.
- `data` has a type only when the analyzer knows it: the `type` field of the entry (section 3.4) is the
  `ui/services.json` method's declared return type when that is `int`, `float`, `bool` or `string`, and
  is omitted for any other result (a record, `void`). A `.data` read of an untyped resource validates
  but is `TESSERA_CODEGEN_UNSUPPORTED`; `.error` has no generated form yet. The key is not checked
  against the method's parameters in V1: binding a key to a fetch is the host's job.

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

1. `DataStatus::Stale` is declared and V1 never produces it. `useResource` MUST read `epoch`, not
   `Stale` (decision 1); the enumerator stays so a later release can define it without an ABI change.
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
  "type": "string",
  "key": { "kind": "record", "fields": { "query": { "kind": "ref", "id": "state::query" } } },
  "source": { "file": "ui/Feed.tsx", "offset": 120, "length": 60, "line": 6, "column": 3,
              "endLine": 6, "endColumn": 63 } }
```

- Reference ids `res::<name>.status`, `res::<name>.data` and `res::<name>.error` resolve in
  expressions; `<name>` is the local binding name, or `res::<method>_<n>` for a bare call, counted from
  1 (the rule of `beh::` in [`behavior-hooks-v1.md`](behavior-hooks-v1.md) section 5).
- `type` is optional and written only when known (section 3.1); a value other than `int`, `float`,
  `bool` or `string` is `TESSERA_IR_UNSUPPORTED`.
- `service` MUST name a declared `svc::` entry; `key` MUST type-check. Entries keep declaration order
  (determinism, [`ir-v1-expr.md`](ir-v1-expr.md) section 11). The field names above are final (decision 2).

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

A boundary covers every suspending read declared in its child subtree **within the same component's own
tree** (decision 3). A read in a child `Component` node is not covered by the parent's boundary: that
component needs a boundary of its own, or its read is `TESSERA_SUSPENSE_NO_BOUNDARY`. A suspending read
with no boundary above it in its own component is that same error. Because the boundary and its reads
live in one generated component, codegen needs no cross-component wiring.

### 4.3 Behavior

| Phase | When | Live subtree |
|---|---|---|
| Content | no covered read suspends | the child subtree |
| Fallback | some covered read suspends | the fallback subtree |

- **While a transition is pending the boundary keeps its old subtree.** A boundary that covers a read
  keyed by a transition-marked setter is held from the first frame the transition is pending: a read
  that is `Loading` with no data does not replace the content that is live with the fallback, and
  releasing the hold (the transition settled) applies rule 1 again. A boundary already showing its
  fallback stays there, because that is the old UI. The component's `pending_sync()` observes the
  mark first, then holds, then publishes the reads (`SuspenseBoundary::set_hold`).
- The subtree's **state is kept** while the fallback shows (ADR-067 item 2): it is detached, not
  destroyed, and returns without a rebuild.
- A boundary tracks at most eight reads (`SuspenseBoundary::kMaxReads`). Eight is a language limit
  (decision 6): a ninth suspending read under one boundary is `TESSERA_SUSPENSE_TOO_MANY_READS`, raised
  by the analyzer and by the validator on hand-built IR, never a silent clamp at run time.
- A read slot that was never registered is a recoverable error `TESSERA_SUSPENSE_UNKNOWN_READ`, never a
  silent drop: ignoring it would show content while loading.
- The swap moves ownership between two slots and re-parents the scene child; it MUST NOT allocate after
  warm-up (section 11).
- The fallback keeps the layout stable and announces loading once, at the bounded rate of
  [`accessibility-v1.md`](accessibility-v1.md) section 10.1 (identical messages within 250 ms).

### 4.4 Versioning

The change is additive; `irVersion` stays `1` ([`ir-v1-expr.md`](ir-v1-expr.md) section 12):

- [`ir-v1.md`](ir-v1.md) section 3: `Suspense` joins the node `kind` list (the table row for `kind`
  next to `ErrorBoundary`). Done in #656 slice 1 (the code already had `NodeKind::Suspense`).
- [`ir-v1-expr.md`](ir-v1-expr.md): a new section 5.2 `Suspense`, after 5.1 `ErrorBoundary` (done in
  #656 slice 1), and the section 12 line that lists additive kinds.
- [`ir-v1.md`](ir-v1.md) section 2 (and the Component table): the optional `resources` list of
  section 3.4, with the `res::` reference ids added to the reference id list (slice 2, with the list).
- [`versioning-v1.md`](versioning-v1.md): no new row (the schema version is unchanged).

An `irVersion` 1 consumer that does not know `Suspense` or `resources` rejects such a module
(`TESSERA_IR_MALFORMED`); the compiler, evaluator and generator ship together, the accepted risk of
`types`/`record`. The `resources` additions to the IR specs land with the `resources` list itself.

## 5. The transition mark and `isPending`

```tsx
const [isPending, startTransition] = useTransition();
<Button onClick={() => startTransition(() => setQuery(next))}>Search</Button>
{isPending ? <Text>Updating</Text> : null}
```

- `useTransition()` takes no arguments and is bound as `const [isPending, startTransition] = ...`
  (ADR-067 item 4; any other binding is `TESSERA_REACT_HOOK_FORM`). It lowers to one `behaviors` entry
  with `"hook": "useTransition"`, `"behavior": "transition"`, no targets and no args, whose one output is
  `isPending`. The entry id is `beh::<pending-name>` (so `beh::isPending`), and a read of the pending name
  lowers to the reference id `beh::<pending-name>.isPending`. `isPending` is a **behavior output**
  ([`behavior-hooks-v1.md`](behavior-hooks-v1.md) section 5): a read-only bool (default `false` before
  the first transition), and a change marks the component updated, at most one update per input
  dispatch.
- `startTransition` is a handler statement taking **one parameterless arrow whose whole body is one state
  setter call** ([`jsx-subset-v1.md`](jsx-subset-v1.md) section 9.2). It lowers to the `set` action the
  setter call would lower to, plus `"transition": true`. V1 marks one write per call; two writes are two
  `startTransition` calls. A dispatch, an animated `.set` or a service call has nothing to mark and is
  rejected, as is any other argument. The result:

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
- **The transition lane** is the set of reads started by a transition-marked setter of the same
  component (decision 4). `useDeferredValue` alone, with no `useTransition` in the component, is never
  pending: it equals its argument. When a transition of the component has a pending read, it holds the
  last settled value until the read settles, then takes the argument's current value.
- **Scalars only** (decision 5): the argument is a `bool`, `int`, `float`, an enum-like `string` literal
  union or an id. A `string` that is not a literal union, a list or a record is rejected with the
  generic `TESSERA_IR_UNSUPPORTED` (deferring them would need a copy, which conflicts with
  allocation-free updates).
- It is a behavior output of the same engine class as the transition, so the result is read-only,
  typed as `value`, updates the component like any output, and is allocation-free (one stored scalar).
- Its meaning is weaker than React's: it is the settled value, not a lower-priority render. Ported code
  gets the same result but not the same timing, and the documentation MUST say so.
- Implemented in #656 slice 3: `const d = useDeferredValue(x)` takes the name of a `useState` value (a
  prop is not accepted yet) and lowers to one `deferred_value` `behaviors` entry whose one arg is `x` and
  whose one output `value` has `x`'s type; `d` reads `beh::d.value`. The generated base keeps one
  `DeferredScalar` member and reads it through an accessor; a marked setter reads it just before it
  assigns, so the settled value is what the UI showed before the change. The evaluator runs the same
  rule in `PendingSession::deferred`.

## 7. `lazy` as identity

- `const Heavy = lazy(() => import("./Heavy"))` resolves the import at compile time and renders
  `Heavy` immediately (ADR-067 item 6). A native app is one binary and fetches no code.
- The call form is exactly that: one parameterless arrow with one `import("<specifier>")`, bound by a
  module-level `const` whose name is the component's own (uppercase) name. The front end treats it as
  `import { Heavy } from "./Heavy"` (the same project-file resolution, cycle rules and diagnostics as a
  named import), so the component named `Heavy` of `./Heavy` is used directly. Anything else (a
  `.then`, an `async` arrow, a computed specifier, a lowercase or differently named binding) is the
  existing `TESSERA_REACT_UNSUPPORTED_API`.
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
| `TESSERA_SUSPENSE_NO_BOUNDARY` | validator, on lowered TSX and on hand-built IR (source position of the `useResource` call) | A `useResource` declared `suspend: true` has no `Suspense` ancestor in its own component, including a read in a child `Component` node (ADR-067 item 2, decision 3). Never a blank screen |
| `TESSERA_SUSPENSE_TOO_MANY_READS` | validator, on lowered TSX and on hand-built IR (source position of the boundary) | More than eight suspending reads under one boundary (decision 6) |
| `TESSERA_IR_ERROR_BOUNDARY` | validator (exists) | A `Suspense` has no `fallback`, or its fallback is conditional or looped |
| `TESSERA_IR_TRANSITION` | validator (exists) | A `transition` mark on an action that is not a state setter |
| `TESSERA_IR_MALFORMED` | validator (exists) | `fallback` on a kind other than `ErrorBoundary`/`Suspense` |
| `TESSERA_SUSPENSE_UNKNOWN_READ` | run time (exists, recoverable `PendingError`) | A read slot was never registered on the boundary |
| `TESSERA_REACT_HOOK_ARG` | analyzer (exists) | `useResource` arguments are not of the declared kind or type, or a key is not pure |

The ADR lists only `TESSERA_SUSPENSE_NO_BOUNDARY`; the spike has no code for the read limit, so
`TESSERA_SUSPENSE_TOO_MANY_READS` is the one addition (decision 6), and nothing else is new
(decision 7). A `startTransition` argument that is not a setter-call arrow is the handler-body rules
(`TESSERA_REACT_BIND_MISSING` for a declared handler, `TESSERA_IR_UNSUPPORTED` for an inline one); a
`lazy` form outside the accepted one is `TESSERA_REACT_UNSUPPORTED_API`; a `useTransition` or
`useResource` call that is not in its accepted form is `TESSERA_REACT_HOOK_FORM`; an unresolvable `lazy`
specifier is the project import codes (`TESSERA_IR_UNRESOLVED`, `TESSERA_IR_UNSUPPORTED`). An
`Error` resource is never routed to an `ErrorBoundary` (decision 9).

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
| Front end, resources and deferred values (#656 slice 3) | `useResource` (forms, typed `data`, bare-call naming), `useDeferredValue`, the rejected forms, and the two `TESSERA_SUSPENSE_*` codes with source positions | `lowering_resource` |
| Boundary hold and deferred rule | a held boundary keeps its content, releasing re-applies the rule; `DeferredScalar` | `pending_model`, `pending_behavior` |
| TSX to generated C++ and the evaluator | one TSX component (resource, `Suspense`, `useTransition`, `useDeferredValue`) agrees with the evaluator after every step, including the held old subtree | `codegen_pending_tsx` |
| Front end (#656 slice 1) | `<Suspense>`, `lazy` and `useTransition` lower to the IR above; the rejected forms are rejected; the module validates; a component without them is unchanged | `lowering_pending`, `tests/golden/ir/pending-ui.json`, `hook_manifest`, `schema` |
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

## 12. Decisions

The questions the draft left open, decided when ADR-067 was Accepted (2026-11-28). The conservative
choice was taken each time; each is folded into the section it changes.

1. **`DataStatus::Stale`** stays in the enumeration. V1 never produces it; reads use `epoch` (section 3.3).
2. **`useResource` IR shape** is the new additive `resources` list of section 3.4, with its field names
   as written there. It does not reuse `environment`.
3. **Coverage across components.** A read is covered only by boundaries in the same component's own
   tree. A read in a child `Component` node needs its own boundary or is
   `TESSERA_SUSPENSE_NO_BOUNDARY` (section 4.2).
4. **The transition lane.** `useDeferredValue` alone is never pending: it equals its argument, unless a
   transition in the same component has a pending read, then it holds the last settled value
   (section 6).
5. **Deferred value storage.** Scalars only in V1 (bool, number, enum, id); any other type is an
   unsupported diagnostic (section 6).
6. **Read limit.** Eight suspending reads per boundary is a language limit with the diagnostic
   `TESSERA_SUSPENSE_TOO_MANY_READS` (sections 4.3 and 9).
7. **Extra diagnostics.** None beyond the spike's codes, `TESSERA_SUSPENSE_NO_BOUNDARY` and the read limit
   of decision 6; generic unsupported and IR codes are reused (section 9).
8. **Key identity.** A key is a scalar or a record of scalars, compared by value and hashed over its
   canonical encoding (section 3.1).
9. **Interaction with `ErrorBoundary`.** None in V1: an `Error` resource is shown through
   `status`/`error` and is not routed to the nearest `ErrorBoundary` ([`error-boundaries-v1.md`](error-boundaries-v1.md)
   section 10 keeps the interaction as a later question).

Decisions taken in #656 slice 3, conservative and recorded where they bind:

10. **`type` of a resource.** The IR has no result type of its own, so the entry gets an optional `type`
    (section 3.4): the manifest method's declared return type when it is `int`, `float`, `bool` or
    `string`, omitted otherwise. Typed `.data` is a scalar in the view (`ResourceState::number` or
    `text`); an untyped `.data` and every `.error` stay `TESSERA_CODEGEN_UNSUPPORTED`.
11. **The `transition` behavior is registered, and the generated base does not attach it.**
    `transition` and `deferred_value` are in the engine registry (the manifest and the registry stay
    one closed set), but `Instance::attach` takes no target-less behavior and the mark is driven by the
    marked setter and the subscription, so the generated base reads its own `TransitionMark` and
    `DeferredScalar`; the registered classes serve a host that drives them by arguments.
12. **`useDeferredValue` takes a `useState` name** (not a prop or an expression) of a bool, number or
    string-literal union; any other argument is `TESSERA_REACT_HOOK_FORM` or `TESSERA_REACT_HOOK_ARG`,
    any other type `TESSERA_IR_UNSUPPORTED` (section 6).
13. **Where the suspense codes are raised.** `TESSERA_SUSPENSE_NO_BOUNDARY` and
    `TESSERA_SUSPENSE_TOO_MANY_READS` come from the IR validator over the lowered component, with the
    source position of the `useResource` call or the boundary; the analyzer does not repeat them, because
    the boundary tree is only known after lowering.
14. **Refresh.** `pending_sync()` publishes resource state to the boundary and the mark; bindings that
    read `res::`, `isPending` or a deferred value are external reads, so a host calls `update()` after a
    sync, as for every other external value (environment, behavior outputs).
15. **Key and method parameters.** A key is checked as a pure scalar or record of scalars; its match with
    the service method's parameters is the host's binding of the fetch in V1.

## 13. Related documents

- [ADR-067](../../decisions/ADR-067-pending-ui-transitions-suspense-lazy.md) (the decision and the spike evidence).
- [`async-data-v1.md`](async-data-v1.md) (resources, epochs, UI integration);
  [`error-boundaries-v1.md`](error-boundaries-v1.md) (the boundary precedent);
  [`behavior-hooks-v1.md`](behavior-hooks-v1.md) (output rules, manifest path, per-hook spec and
  verification map conventions, issues #638, #639, #641);
  [`ir-v1.md`](ir-v1.md) and [`ir-v1-expr.md`](ir-v1-expr.md) (node and versioning conventions);
  [`component-model-v1.md`](component-model-v1.md) (update model); [`accessibility-v1.md`](accessibility-v1.md).
- [`../reference/react-core-coverage.md`](../reference/react-core-coverage.md) rows for
  `useTransition`, `useDeferredValue`, `lazy` and `Suspense`.
