# Background work and lifecycle

Part of the [services design set](README.md). Section 7 is what services must do on suspend and
resume; 11.4 is the AF2 slice that landed for it (`appfw::TaskScope` and the layer-0 timer wake
source), filed under section 11 in the original document because that is where the app-framework
slices were recorded. Both keep their original numbering and heading level, so `section 7` and
`section 11.4` still name these headings. Details: [`../background-work.md`](../background-work.md).

## 7. Background and lifecycle

- Services react to suspend/resume/background/foreground via normalized events.
- Long-running work is cancelled or checkpointed on suspend.
- Network/file handles are released in background when the platform requires it (conditions not
  specified; see [`status-and-open-questions.md`](status-and-open-questions.md) section 10).
- No background work that violates platform policy; document per-platform limits.
- Tests: suspend during load, resume/restore, cancellation, duplicate callbacks.

### 11.4 Background work: `TaskScope` and the timer wake source (AF2, issue #174)

The roadmap row pairs two things and they land in different layers, which is the first thing to know
about this slice:

| Piece | Layer | Where | Why |
|---|---|---|---|
| `appfw::TaskScope` | 1 | `include/tessera/appfw/task_scope.hpp` | it is an app-framework concept: bounded children of a service, window or component |
| `runtime::TimerQueue` | 0 | `include/tessera/runtime/timer_queue.hpp` | a timer is a wake source the loop schedules against, not an app concept |

`TaskScope` guarantees, in the ADR's own order: `spawn()` is the only start path, so no detached task
is expressible; the child table is fixed-capacity and a full scope refuses instead of growing;
`cancel()` flips one token that every child checks before its next resume; `close()` records a
deadline and returns, and `tick()` reports `Open`/`Closing`/`Closed`/`TimedOut`, so shutdown is
tick-driven and never blocks the UI thread. Scopes nest through the parent's `CancellationToken`,
with the registration held weakly so an outlived child scope is never written through.

**Cancellation while a child is suspended** is the case that decides the shape, and the rule is that
a suspended coroutine frame is never destroyed by the scope. Each child runs under a token-free
driver coroutine that holds the scope's bookkeeping weakly; the frames are owned by the task graph,
so destroying the `TaskScope` cancels and drops the handle without dangling. A child that refuses to
finish leaves a bounded, visible cycle (`live()` stays non-zero and `tick()` reports `TimedOut`)
rather than being destroyed under an awaitable that may still hold its handle. The reason a driver's
own context must be token-free is the same split: a tokened driver would be skipped exactly when its
child is cancelled, and the driver is what retires the slot. AF1 records the identical reasoning for
service attempts in `src/appfw/service_bridge.hpp`.

The timer side is a layer-0 seam in the strict sense of ADR-050 section 1 rule 1: nothing in
`timer_queue.hpp` or `runtime_loop.hpp` names `appfw`, a service or a store. `RuntimeLoop::tick()`
fires due timers after the completion drain and before the fixed step, and never turns a fire into a
frame request; `next_timer_wait_seconds()` is the pure query a host joins with the pacer deadline, so
an idle app parks until the earlier of the two. Firing order is `(deadline, registration sequence)`,
which is what makes "timers fire in order" deterministic for equal deadlines, and a periodic timer
that missed several periods fires once and advances past `now` instead of bursting.

The acceptance evidence is counters, never elapsed time: with only a far timer pending, a
`ManualClock`-driven loop is idle at `t = 0` with `next_timer_wait_seconds() == 10.0`, presents zero
frames, and costs exactly one further tick after the clock reaches the deadline. Details, the
re-entrancy rules and what the slice deliberately does not wire (the `WindowHost` park join and
`Application`'s root scope) are in [`background-work.md`](../background-work.md).
