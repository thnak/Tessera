# Observable: coalesced app state over `UpdateScheduler`

Status: implemented (issue #176, roadmap row AF4 of
[ADR-050 section 6](../../decisions/ADR-050/background-work-and-app-state.md#6-app-wide-state)). Code:
[`include/tessera/appfw/observable.hpp`](../../include/tessera/appfw/observable.hpp). Tests:
`appfw_observable` (`tests/unit/appfw_observable_tests.cpp` and
`tests/unit/appfw_observable_allocation.cpp`).

Normative companions: [ADR-050 section 6](../../decisions/ADR-050/background-work-and-app-state.md#6-app-wide-state)
and 10, [`app-framework-open-questions.md`](app-framework-open-questions.md) section 6,
[`animation.md`](animation.md) section 6 (the sampler that shares this scheduler), and
[`dirty-rendering.md`](dirty-rendering.md) section 2 (the domains a notification raises).

## 1. What it is, and why it is not a second batching mechanism

`appfw::Observable<T>` is a value plus a version, owned by an app-scoped object (typically a
service), that tells its observers about a change **once per scheduler flush** instead of once per
write. A hover handler that runs on every mouse move may write hundreds of times between two
frames; the frame after them sees one notification.

It adds no timer, queue or batch of its own. An observable owns one `ui::UpdateTicket`, the same
primitive a generated component uses, attaches it to the window's `UpdateScheduler` when it is
constructed, and `set()` calls `UpdateTicket::schedule()`. Everything the scheduler already
guarantees then applies unchanged: one dirty edge wakes the render-on-demand loop, updates run in
attach order, a write from inside an update re-runs a pass, and the pass budget
(`kDefaultMaxPasses`, 8) bounds a cycle.

The ticket is a **member** of the observable and the type is **not movable**
(`static_assert`ed in the tests), because `UpdateScheduler::attach` stores the ticket's address and
`UpdateTicket` requires that address to stay stable while attached. The cost is that an observable
is constructed in place, at start/mount, exactly like the components it serves.

```cpp
tessera::ui::UpdateScheduler& scheduler = window.update_scheduler();
appfw::Observable<Route> route(scheduler, Route::home());      // start(): one attach
auto subscription = route.subscribe(&Screen::on_route, this);   // mount(): one node

// Anywhere on the UI thread, as often as the input rate demands:
route.set(Route::message(id));      // compare + assign + one dirty bit; no allocation, no notify
```

## 2. The two acceptance clauses

### 2.1 One flush per tick, whatever the number of writes

N `set()` calls between two flushes produce exactly ONE notification per observer. The test asserts
it literally, with N = 500, and then checks the same contract at N = 1 and N = 0:

```cpp
for (int i = 1; i <= 500; ++i) { observable.set(i); }
// ... recorder.calls == 0 ...
scheduler.flush();
// recorder.calls == 1     and stats.updated == 1     and the value delivered is the last one
```

Because the notification carries the **live** value, an observer never sees an intermediate one;
there is no queue of pending values to replay. Coalescing is the reason `set()` is safe from a
per-mouse-move handler, not a side effect.

An unchanged value is cheaper than that: `set()` compares first, so re-asserting the current value
neither assigns nor dirties the scheduler, and the frame after it does no work at all.

### 2.2 No allocation on `set()`

Measured, not asserted by inspection. `tests/unit/appfw_observable_allocation.cpp` replaces the
global `operator new`/`new[]`/`delete` pair (the technique of
`tests/unit/chart_series_ring_allocation.cpp`) so every allocation in that binary is counted. On a
live observable with a callback observer and a ticket observer attached:

| Window | Allocations | Bytes |
|---|---:|---:|
| 10 000 `set()` calls | 0 | 0 |
| the flush that coalesced them (2 updates, 1 notification) | 0 | 0 |
| 1 000 `set()` calls with an unchanged value | 0 | 0 |
| 1 000 idle flushes | 0 | 0 |
| unsubscribing both links | 0 | 0 |

The measured window also asserts that the work really happened (exactly one observer call, one
ticket update, the value at the last write), so a zero is not the result of a loop that was elided.
Subscribing is the one allocation point, and the test asserts that it does allocate: that is mount
work, and it keeps "zero on `set()`" from being a vacuous claim about a path that allocates nowhere.

The mechanisms that make the zero real:

- observers are `void (*)(void*, const T&)` plus an explicit context, not `std::function`, so
  notifying never type-erases a closure;
- the observer registry is a `std::vector<ObserverNode*>` that only grows on `subscribe()`;
- an unsubscribe during a notification nulls a slot and compacts in place after the pass, so
  removal neither allocates nor shifts storage under the running loop;
- the scheduler allocates nothing after `reserve()` (`update_scheduler.hpp`).

## 3. Delivery model: `const T&`, bound to the live value

Observers receive a reference to the observable's own value, not a copy, and no copy is made per
observer. Observers run synchronously on the UI thread inside the flush, so a copy of a large `T`
(`Route`, a message list, a document snapshot) would be paid on the hot path to hand out a value
the frame is about to read through the same object. A test uses a copy-counting `T` and asserts
zero copies across 19 writes and the notification pass, and that the address the observer saw is
`&observable.get()`.

Two consequences belong to the contract, not to the implementation:

1. **A callback may not keep the reference.** It is valid only for the duration of the call. An
   observer that needs the value later must copy it deliberately, which makes the cost visible at
   the call site.
2. **A write from a callback is visible immediately.** `set()` assigns before the remaining
   observers run, so they see the new value; the write raises the dirty bit, and the scheduler
   delivers one more notification pass rather than notifying recursively. That pass is bounded by
   `kDefaultMaxPasses`; a self-triggering observer is reported as `pass_limit_hit` in the flush
   stats instead of spinning. The alternative (snapshot the value per pass) would allocate or copy
   per notification, which is exactly what the delivery model exists to avoid.
3. **`T` must be copy-assignable and equality-comparable.** The first is what `set()` needs; the
   second is what coalescing needs, and a missing `operator==` is a `static_assert` with that
   message rather than a silent "every write notifies".

## 4. Unsubscribe during a flush: immediate removal, deferred reclamation

An observer removed during a notification (by itself, or by an earlier observer) is **not called
again in that pass or in any later one**. Removal takes effect at once; only the memory is
reclaimed at the end of the pass.

The `Subscription` is a move-only RAII handle: its destructor unsubscribes, moving it transfers the
link, and releasing an already-released handle is a no-op. `subscribe()` has two forms:

- `subscribe(callback, context)` — a callback observer;
- `subscribe(UpdateTicket&)` — ADR-050 section 6's form, where the observer is another component, so
  its `update()` runs at the flush. The same dirty bit coalesces it: 50 `set()` calls against a
  ticket observer ran its `update()` once (asserted).

**Why immediate removal with deferred reclamation, and not deferred removal.** A component that
unmounts from inside a notification (an error boundary closing a subtree, a list item reacting to a
routing change) must not be called again after it says it is gone: a deferred model gives every
still-to-run observer one more call into a destructed context, which is a use-after-free the
framework would be handing out. Removing the link immediately makes the guarantee unconditional and
cheap to state.

**Why the slot is nulled instead of erased mid-pass.** Erasing shifts the vector under the running
index loop, so an observer after the removed one would be skipped — the same hazard
`UpdateScheduler::detach` already handles for a ticket detached mid-flush, and it solves it the
same way: a null hole, then drop the holes when the flush ends. Nulling also keeps the unlink O(n)
in the registry size only, with no `memmove` on the frame path. The pass snapshots the registry size
at entry, so an observer **subscribed** from a callback does not receive the notification that is
already in flight.

**Lifetime.** The links are non-owning: each `ObserverNode` is owned by its `Subscription`, and the
observable only holds pointers to them. That makes both orders safe. A subscription may outlive its
observable: the observable clears its node list in its destructor and marks each node ownerless, so
a later release is a no-op instead of a dangling unlink. An observable may also die while
subscriptions are still alive, which is the same code path and does not leak, because the
`Subscription` destructor frees its own node. What a `Subscription` must not do is outlive the
*context* its callback captures; that is the caller's contract, and it is why a component releases
its subscriptions in `unmount()` rather than leaving them to the window's teardown.

## 5. What this slice deliberately does not do

- **No debug change trace.** ADR-050 section 6 and
  [`app-framework-open-questions.md`](app-framework-open-questions.md) section 6.1 describe an
  optional bounded devtools trace of `set()` (stable name, version, frame index) compiled out of
  release. The observable carries the version (`version()`) this needs; the trace itself belongs
  with the devtools slice and its diagnostics registry.
- **No `Computed<T>` / derived values.** Deferred by ADR-050 section 6.
- **No TSX surface.** `useObservable(svc.member)` is AF5 codegen work (open question 6's answer);
  this slice is the C++ contract the lowering will call into.
- **No service wiring.** No service owns an observable yet; nav, data and permissions adopt it in
  their own slices.

## 6. Open question for the owner

**Is the `Observable` an owner of its own ticket, or must every observable be observed only through
a component's ticket?** ADR-050 section 6 says `subscribe(UpdateTicket&)`, which this slice supports
and which is what codegen will emit. The owned-ticket form is what makes "one flush per tick"
true for a service-level observable that no component subscribes to directly, and it is what the
acceptance clause is stated against. Both share one scheduler and one dirty bit, so the choice is
additive; the question is whether the framework should ever require a component ticket (today it
does not), and that is a contract question rather than an implementation one.

## 7. What one state change costs: measured update fan-out (issue #440)

`UpdateScheduler` coalesces writes into one `update()` per queued component per flush, but a generated
`update()` also calls `update()` on every static child instance and every live per-item instance it
owns (`emit_lifecycle.cpp`), and each of those components attaches its **own** ticket too. The numbers
below are measured by `codegen_update_fanout` (`tests/unit/codegen_update_fanout_tests.cpp`) on the
committed `instances/{Page,Row}.g.hpp`: the static `Row` plus a keyed list of `n` per-item `Row`
instances, with a counting developer class for `Row` (`fixtures/update_count/Row.hpp`).
`ticket` is `UpdateFlushStats::updated`; `direct` is `Row::update()` calls made by an owner's generated
`update()`. Every scenario is one flush and **one pass**.

| Scenario | ticket | direct | `Row` runs in total |
|---|---|---|---|
| The static `Row`'s own state changes | 1 | 0 | 1 |
| The page's state changes (it feeds no `Row`) | 1 | n + 1 | n + 1 |
| The page and the static `Row` are both dirty | 2 | n + 1 | n + 2 |

Rows 1 to 3 held identically at n = 10, 100 and 1000. Reading them:

- **A change in a child's own state is cheap and local.** Only that child's ticket runs; the parent and
  the siblings are not touched. State kept per item is therefore the cheap shape.
- **A change in the owner's state fans out to every child, whether or not it feeds them.** n + 1
  `update()` calls for one setter, each re-hashing its component's bindings. The hash gate keeps the
  *output* small (only changed nodes are marked dirty), but the CPU is O(n x bindings). State kept in a
  parent array is the expensive shape, and it is the one React-style code writes by default.
- **A parent and a child dirty in the same flush run the child twice.** The parent calls
  `child.update()` directly, which does not clear the child's queued mark, so the scheduler then runs
  the child's ticket as well. The second run is hash-gated no-ops but it is counted in `updated`.

Not measured here: the per-binding hash evaluations (not observable from the developer class), a
parent array whose items feed the `Row`s' props, and virtual collections, whose per-item content pass
is separate from `update()`. Per-item `Row` state is private to the generated code, so the "child's own
state" row uses the one static child as the stand-in.

No fix is made or proposed here. Tracking which state each binding reads, or skipping a child whose
props hash did not change, would remove the fan-out and the double run, but it changes the codegen
contract and needs an owner decision (see #440).
