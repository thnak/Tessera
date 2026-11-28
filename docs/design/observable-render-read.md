# Reading a service observable in render (`useObservable`)

Status: implemented ([#1044](https://github.com/thnak/Tessera/issues/1044) and
[#1045](https://github.com/thnak/Tessera/issues/1045) need it). The decision is the owner-accepted
answer of [`app-framework-open-questions.md`](app-framework-open-questions.md) section 6.2 (option
(c), an explicit hook), which is ADR-050 open question 6; no new ADR, because nothing here reverses
or adds to that answer. This note records how the compiler realizes it. The normative parts live in
[`services-manifest-v1.md`](../specs/services-manifest-v1.md) (the `observables` key),
[`ir-v1.md`](../specs/ir-v1.md) (the `observables` list) and [`ir-v1-expr.md`](../specs/ir-v1-expr.md)
section 4.5 (the `obs::` reads).

```tsx
export function Header() {
  const nav = useService<Navigator>();
  const title = useObservable(nav.title);          // Observable<string>, listed in services.json
  const route = useObservable(nav.route);          // Observable<Route>, a declared record
  return <Text>{title} / {route.depth}</Text>;
}
```

## 1. Why a hook and not an implicit read

An implicit read (any mention of a manifest-listed observable) would need dependency tracking in the
compiler and would subscribe because of a read buried in a callback. The hook makes the subscription
visible in source, follows the rules of hooks (top level, never in a handler), and fixes the
subscription set at mount: no dynamic tracking, which is why signals were rejected in ADR-050
section 6. A derived value stays a C++ concern (`Computed<T>` is deferred by the same ADR).

## 2. Surface

- **Manifest.** A service lists `observables: [{ "name", "type" }]`. `type` is an IR value type (never
  `void`); a record must be declared in the module. The C++ interface exposes each as
  `tessera::appfw::Observable<T>& name()`: non-const, because subscribing registers an observer.
  A member name is unique across the observables and methods of one service.
- **TSX.** `const x = useObservable(svc.member)`: one `const` name, top level, and the argument is a
  direct member access on a `useService` binding declared above it. The value is read-only; writes
  stay service methods called from handlers or effects. A scalar is read as `x`, a record as
  `x.field`. `services.d.ts` types the member as `readonly` and declares
  `useObservable<T>(source: T): T`, so an editor checks the same shape.
- **Not covered yet** (no test, so no promise): an observable in an effect's dependency list (the
  dependency check rejects it today), a read inside a list item template, `list<T>` and
  `optional<T>` values read as a whole, and a Studio mock of an observable (the evaluator reads any
  `obs::` id from its scope, so a host only has to put one there).

## 3. Pipeline

| Stage | What it does |
|---|---|
| Rules (`rules/src/observable.cpp`) | `check_observable` checks the form and declares `SymbolKind::Observable`; `check_observable_services` resolves the binding and the member against the manifest and records the type. Errors reuse `TESSERA_REACT_HOOK_FORM`, `TESSERA_REACT_HOOK_ARG` and `TESSERA_REACT_HOOK_ORDER`; the message names the members the manifest lists |
| Lowering | one `ir::Observable` (`obs::<name>`, `svc::` id, member, type); `x` and `x.field` resolve to `obs::x` and `obs::x.field` like a record-typed state |
| IR validation | `obs::` ids are declared with their type, so a dotted read is member-checked; an entry naming a service the component does not declare is `TESSERA_IR_UNRESOLVED` |
| Codegen | a cached `Observable<T>*` and a `Subscription` per entry, `attach_services()` caches the address, `mount()` subscribes, `unmount()` releases, reads go through `obs_<name>_value()` |
| Evaluator | `obs::x` is an ordinary scope entry (a record is a record value), so the differential suite feeds the same values to both sides |

## 4. Runtime behavior

- **One bit per observable.** Every binding that reads `obs::x` (whole or by field) depends on the
  dirty bit `obs::x`. The subscription cannot tell which field of a record changed, so a record is one
  bit; a binding that reads another cell or another observable is not re-run (pinned by the runtime
  test, which writes `unread` 50 times and checks the `title` text was not touched).
- **Coalescing is the observable's.** The observable notifies once per scheduler flush however many
  `set()` calls happened (`observable.md`). The callback marks the bit and calls `schedule_update()`;
  the component's ticket runs in the same flush or the next pass. An unchanged value schedules
  nothing, so a static app stays idle.
- **Before attach.** `build()` may run before `mount(context)`; `obs_<name>_value()` then returns the
  zero value of `T`, and `mount()` without a context subscribes nothing (the pointer is null).
- **No copy.** Reads bind `const T&` to the live value. A `T` should be a value type or an immutable
  snapshot handle; the callback may not keep the reference.
- **Threads.** Writes and notifications happen on the UI thread, inside the flush, so every component
  sees one value per frame.

## 5. What this unblocks

`useParams`/`useLocation` ([#1044](https://github.com/thnak/Tessera/issues/1044)) are a read of the
route observable of the router service, and the zustand-shaped `create` hook
([#1045](https://github.com/thnak/Tessera/issues/1045)) is a store over `Observable<T>`. Both slices
(the `RouterService` and the store shim spec) are not in this tree yet, so they stay with those
issues; the compiler side they need is this note.
