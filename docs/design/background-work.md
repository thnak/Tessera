# Background work: `TaskScope` and the timer wake source

Status: implemented (AF2, issue #174). ADR-050 section 5 is the decision; this note records the shape
that landed, the two mechanisms it reuses from layer 0, and the concurrency rules a caller has to
respect.

Companions: [`services/common-contract.md`](services/common-contract.md#11-implemented-shape-af1-issue-173)
section 1.1 for the coroutine shape AF1 established,
[`async.md`](async.md) for the completion queue and cancellation primitives, and
[`game-loop.md`](game-loop.md) section 9 for the idle policy a host applies to the park.

## 1. What the slice owns

| Piece | Layer | Header | Why there |
|---|---|---|---|
| `TaskScope` | 1 (`tessera_appfw_core`) | `include/tessera/appfw/task_scope.hpp` | it names services and app lifecycle concepts, so it cannot live in layer 0 |
| `TimerQueue` | 0 (`tessera_runtime`) | `include/tessera/runtime/timer_queue.hpp` | a timer is a wake source the loop must schedule against, not an app concept |
| `RuntimeLoop` timer join | 0 | `src/runtime/runtime_loop.cpp` | the loop already owns the tick; the join is one call and one query |

ADR-050 section 1 rule 1 allows exactly this: layer 0 gains *seams, not concepts*. Neither
`timer_queue.hpp` nor `runtime_loop.hpp` names `appfw`, a service, a store or an app state. The
dependency arrow stays `core <- runtime <- appfw_core`.

## 2. `TaskScope`: structured cancellation

A scope is a bounded table of live children plus one `CancellationSource`:

```cpp
appfw::TaskScope scope(clock, completions, appfw::TaskScopeConfig{16});
scope.spawn(load_mail(scope.context(), *this));   // the scope's token is in the context
scope.close(clock.now_seconds() + 0.5);           // cancel + a deadline; returns immediately
while (scope.tick() == appfw::TaskScopeCloseState::Closing) { /* next frame */ }
```

Guarantees, each with the mechanism that provides it:

1. **No detached task is expressible.** `spawn()` is the only start path, and it starts the task
   through the scope's own driver coroutine. There is no "fire and forget" overload.
2. **Bounded.** A full scope refuses the spawn (`false`) and counts it; it never grows the table.
   `TaskScopeConfig::capacity` is fixed at construction and storage is one allocation there.
3. **`cancel()` is cooperative and non-blocking.** It flips the token. `Task` checks the token
   before every resume and finishes a cancelled child with `TaskCancelled`, so a child stops at its
   next resume and never between two statements of its own body. The scope never runs, joins or
   waits for a child.
4. **`close()` is deadline-bounded and tick-driven.** It cancels, records an absolute deadline on
   the injected clock and returns. `tick()` reports `Open`, `Closing`, `Closed` or `TimedOut`; a
   host calls it once per frame, so shutdown progresses without ever blocking the UI thread.
5. **A child's failure is a counter, not an exception.** An escaped exception increments `failed()`;
   an exit through `TaskCancelled` increments `cancelled_children()`. Either way the slot is
   retired, and nothing propagates into the frame loop (ADR-025).
6. **Scopes nest.** A child scope takes its parent's `CancellationToken`; cancelling the parent
   cancels the child. The registration holds the child's state weakly, so a parent that outlives a
   child scope cannot keep it alive or write through a dangling pointer.

### 2.1 Cancellation while a child is suspended

This is the case that decides the design, and the rule is: **never destroy a coroutine frame that a
live callback may still resume.**

A child is wrapped in a token-free driver coroutine that awaits it. The driver holds the scope's
bookkeeping through a `weak_ptr`, and the task graph itself holds the frames (a `TaskState` owns its
frame; a child holds its awaiter's state as the continuation). Three consequences:

- destroying the `TaskScope` while a child is suspended cancels the scope and drops only the
  handle; the suspended frames stay owned by the task graph, so nothing dangles;
- a driver that outlives the scope finds the bookkeeping gone and returns without touching it;
- a child that ignores its token keeps its frame alive, so `live()` never reaches zero and
  `tick()` reports `TimedOut`. That is visible in the counters instead of being a silent crash.

A child parked on an awaitable that can never resume it is the one case where the pair is not
retained: the awaitable declares that through `runtime::detail::declare_no_resume`, the child drops
its reference to the driver, and the pair is freed when the scope releases the slot
([ADR-069](../../decisions/ADR-069-awaitable-declares-no-resume.md)). A child suspended on anything
that can still resume it - a queue hop, a callback, a timer - keeps the cycle, because destroying
its frame would let a live resume touch freed memory.

The driver's own `TaskContext` is deliberately token-free. A tokened driver would be skipped by
`TaskStateBase::resume_from_queue` exactly when its child is cancelled, and the driver is what
retires the slot and reports the drain. AF1 records the same reasoning for service attempts in
`src/appfw/service_bridge.hpp`.

The cost is honest and bounded: an abandoned child and its driver reference each other (the driver's
awaiter holds the child's state; the child holds the driver's state as its continuation), so a child
that ignores its token leaves a pair of frames in a cycle that is only broken when it finally
finishes. It is bounded by the scope's `capacity`, and it is visible — `live()` never reaches zero
and `tick()` reports `TimedOut`. The alternative, destroying the frame, is not safe in general: the
awaitable the child is suspended on may hold a bare `coroutine_handle` and resume it later (the
queue-hop fixture in the tests does exactly that), so destroying the frame risks resuming freed
memory. Because the scope cannot tell which kind of await it is sitting on, it never destroys a
suspended frame. AF1 accepts the same trade-off for an abandoned service stop.

## 3. The timer wake source

`TimerQueue` is fixed-capacity and stores `{deadline, period, sequence, generation, callback}` per
slot. Scheduling reuses an inactive slot and bumps its generation, so a stale `TimerId` cancels
nothing rather than cancelling whichever timer took the slot.

### 3.1 Ordering

Firing order is `(deadline, registration sequence)` — earliest deadline first, and equal deadlines
break by registration order, deterministically:

```cpp
// Registered in this order, all due at t = 5:
queue.schedule_one_shot(5.0, [&] { fired.push_back(3); });
queue.schedule_one_shot(5.0, [&] { fired.push_back(1); });
queue.schedule_one_shot(5.0, [&] { fired.push_back(2); });
queue.fire_due(5.0);
// fired == {3, 1, 2}
```

The sequence counter is the tiebreaker, not the slot index: a one-slot queue that fires and is
reused must still put the newer timer after the older one, which slot order alone would not do.

### 3.2 Periodic timers coalesce

A periodic timer's first fire is its deadline; later fires are whole periods after it. If `now` is
several periods late, the timer **fires once** and its deadline advances to the first period
strictly after `now`. A loop that parked through a long gap produces one callback, never a burst of
catch-up work — which is what makes a dropped-frame period cheap instead of a stall.

### 3.3 The join with the pacer, and what "parks" means

`RuntimeLoop::tick()` fires every timer due at the instant it read from its clock, after the
completion drain and before the fixed step. Firing a timer never sets `frame_requested_`: a timer is
a wake source, not a render cause (ADR-050 section 5 item 2). Its callback schedules work like any
completion, and the next tick's drain runs it.

`RuntimeLoop::next_timer_wait_seconds()` is the query a host joins with the pacer deadline:

```cpp
const double park = std::min(app::pacer_wait_seconds(pacer.ready(now)),
                             loop.next_timer_wait_seconds());
```

It answers `kNoDeadline` when nothing is scheduled (or no queue is attached) and `0` for a timer
that is already due, so a host that clamps to the minimum can never sleep past a timer. It is a
pure query: it neither fires a timer nor advances time.

**The acceptance evidence is counters, not elapsed time.** With only a far timer pending, a
`ManualClock`-driven loop reports `should_render == false` and `idle == true` on the tick at `t = 0`,
`next_timer_wait_seconds() == 10.0`, zero presents, and then exactly one more tick after the clock
advances to the deadline — two ticks for ten seconds, one fire, zero presents. "No polling" is the
statement that the host's wait came from the deadline query rather than a fixed interval, which the
same test asserts by never ticking between the two wakes. See
`tests/unit/runtime_timer_tests.cpp`; `an_idle_loop_with_a_far_timer_parks` is the case that
carries the clause.

### 3.4 Re-entrancy and errors

- A callback may schedule or cancel any timer, including itself; the one running has already had its
  callback moved out, and its slot is re-armed only if it is still the same generation.
- A callback that calls `fire_due` again fires nothing: the outer call owns the iteration. A
  re-entrancy guard also clears on the exception path, so one throwing callback cannot permanently
  latch the queue shut.
- A non-finite `now` fires nothing rather than throwing, because `fire_due` runs on the frame path.
  Scheduling is *not* a frame path: a non-finite deadline, a non-positive period or an empty
  callback throws `std::invalid_argument`, matching `FramePacer`'s config validation.

## 4. What this slice does not own

- **Wiring the join into `WindowHost`.** The driver still parks on the pacer deadline only; adding
  the timer term is the host slice that first schedules a timer, and ADR-050 section 5 item 2 does
  not require a timer to exist yet. `app/idle_policy.hpp` is unchanged and the documented clamp
  still applies to whichever deadline is nearer.
- **`Application`'s root scope.** AF3's `Application` borrows a `ServiceRunner`; it does not yet own
  the app `TaskScope` ADR-050 section 2 names, because no service schedules a timer or spawns a task
  until AF5/AF7. Adding a scope with no caller would be untestable dead wiring, the same reasoning
  [`services/app-lifecycle-rulings.md`](services/app-lifecycle-rulings.md#111-who-marks-the-in-frame-lookup-region-the-appfw-tick-wrapper) section 11.1 records for the
  in-frame mark.
- **Pausing timers on `Suspended`.** ADR-050 section 5 item 4 assigns that to the mobile lifecycle
  work; the queue's `clear()` and per-timer `cancel()` are the primitives it will use.
