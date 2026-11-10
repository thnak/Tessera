# Error boundaries: design note

Status: runtime slice implemented (`tessera_boundary`); codegen integration, the root boundary UI,
and crash-report wiring are pending. Decision: [ADR-025](../../decisions/ADR-025-error-boundaries.md).
Normative contract: [`../specs/error-boundaries-v1.md`](../specs/error-boundaries-v1.md). This note
explains the rationale and the mechanics; where it and the spec differ, the spec wins.

## 1. What this note covers

The boundary is the only place where a synchronous exception from generated component code turns
into a value. This note records why the slice is shaped the way it is: the routing table, the
RTTI-free catch order, the retry state machine, the single cleanup seam, what is deliberately left
uncaught, and the test map. API-by-API ownership lives in the spec (section 11), not here.

## 2. Shape of the slice

`tessera_boundary` (`include/tessera/ui/boundary/`, `src/ui/boundary/`) is a static library with no
scene, GPU, clock, or thread dependency. The only dependency is `tessera_diagnostics`, used for
`redact_paths` when a report is built. That keeps the guard usable from generated entry points
without linking the renderer.

Files:

| File | Responsibility |
|---|---|
| `error.hpp` / `error.cpp` | `Error` value, `ErrorClass`, marker exceptions, bounded conversion |
| `report.hpp` / `report.cpp` | `BoundaryCode`, `Report`, `ReportSink`, bounded `MemoryReportSink`, redaction |
| `retry_policy.hpp` / `retry_policy.cpp` | the pure retry state machine and the published schedule |
| `boundary.hpp` / `boundary.cpp` | registration, routing, reset, unmount, the cleanup seam |
| `boundary_catch.cpp` | the catch router: fallback, escalation, async values, cancellation |
| `guard.hpp` / `guard.cpp` | `run_guarded` and the fixed-order throw-to-value conversion |

Determinism rules:

- No clock read and no sleep. All time is fixed-step seconds injected by the caller; a retry only
  compares that value with a stored deadline.
- No threads. The table is UI-thread-owned and is not synchronized; async failures arrive on the UI
  thread at a frame boundary as values.
- No RTTI on the failure path (section 4).
- Allocation happens only when a failure is being handled (exception paths, report strings, the
  growing registry). A successful guarded run allocates nothing.
- Capacity is bounded: `kMaxBoundaries` (64) registrations, `kMaxCodeBytes` (64), and
  `kMaxMessageBytes` (512) per report; the default MemoryReportSink ring is bounded too.

## 3. Routing classes

`BoundaryTable::catch_error`, `fallback_failed`, and `report_async` answer two questions: what
class is this failure, and which owner takes it? The class decides whether a component boundary is
even allowed to isolate it.

| Input | Target | `BoundaryDisposition` | State change | Report |
|---|---|---|---|---|
| Recoverable, nearest boundary in `Normal` | that boundary | `Fallback` | child detached, fallback live | `CAUGHT` to onError/diagnostics, once |
| Recoverable, no catcher (none registered, all in `Fallback`, or unmounted) | root handler | `Escalated` | none | `CAUGHT` at root |
| `Fatal` (`bad_alloc`, abort, process-level) | root handler | `Escalated` | none | `CAUGHT` at root |
| `Platform` (HAL/GPU/device loss) | root handler | `Escalated` | none | `CAUGHT` at root |
| Event throw, `catchEvents = false` | root handler | `Escalated` | none | `CAUGHT` at root |
| Cancellation **exception signal** | root handler | `Cancelled` | none | `CANCELLED` notification, once, counter incremented |
| Cancellation **async value** | none | `Cancelled` | none | none |
| Recoverable async value, generation current | catcher | `Fallback` | child detached, fallback live | `ASYNC`, no retry |
| Recoverable async value, stale id/generation | diagnostics only | `Dropped` | none | `ASYNC` with `dropped = true` |
| Fallback (or onError) failure | nearest parent boundary, else root | `Fallback` / `Escalated` | fallback disposed | `FALLBACK_FAILED`, never retried |

Two invariants fall out of the table:

- A boundary in `Fallback` is no longer a catcher for its own child (`catcher()` walks to the
  parent). That is what stops "fallback fails, fallback shows, fallback fails" from being an
  infinite loop inside one boundary.
- Every row that ends in a handler produces exactly one report. The only row without a report is
  the async cancellation value, which is a control outcome its owner already produced.

`catch_error` is the synchronous entry point: it is what `run_guarded` calls after converting a
throw. `report_async` is the value entry point and carries the owning boundary's generation so a
late completion for a rebuilt child cannot resurrect a dead subtree.

## 4. The RTTI-free catch order

`capture_current_error` rethrows the active exception with `throw;` and converts it through a
fixed clause order:

```text
CancellationSignal   -> ErrorClass::Cancellation
PlatformFailure      -> ErrorClass::Platform
std::bad_alloc       -> ErrorClass::Fatal
std::exception       -> ErrorClass::Recoverable
...                  -> ErrorClass::Recoverable ("unknown_exception")
```

The order encodes the class, so no `typeid`, `dynamic_cast`, or type-name comparison runs while an
exception is in flight. Two constraints make the order non-negotiable:

1. `CancellationSignal` and `PlatformFailure` derive from `std::exception`, so they must be caught
   before it; otherwise a cancellation would be classified as a recoverable error and show a
   fallback.
2. `std::bad_alloc` derives from `std::exception` too, and must be caught before it to stay
   `Fatal`; an OOM must never be isolated behind a fallback that itself needs memory.

`run_guarded` has one `try` and one `catch (...)`: it runs the callable, and on a throw calls
`capture_current_error`, stamps the generated source span, and hands the value to `catch_error`.
It is deliberately **not** `noexcept` -- a throw inside a `noexcept` function terminates, and a
failure of the catch point itself (for example OOM while routing) must reach the process handler
rather than abort a frame.

`capture_current_error` must be called from inside a catch handler (it rethrows); with no active
exception it terminates. That is the documented conversion step for an FFI or thread edge, where a
bridge returns `Result<T, Error>` instead of throwing across the ABI.

## 5. Retry state machine

`RetryPolicy` is a pure state machine over injected time. States:

```text
Idle --failure and budget left--> Waiting --deadline reached--> Ready
  ^                                  |                            |
  |                                  +-- budget spent --> Disabled (loop guard)
  +-------------------------- reset ----------------------------+
```

- `Idle`: nothing pending; a failure may schedule a retry.
- `Waiting`: a retry is scheduled at `deadline_seconds = now + delay_seconds`.
- `Ready`: the caller's time reached the deadline; the table disposes the fallback, reopens the
  boundary, clears the stored error, and bumps the generation so late async values from the failed
  child are dropped.
- `Disabled`: the last failure could not schedule a retry -- auto-retry is off
  (`maxAutoRetries == 0`) or the budget is spent; `loop` is set only in the latter case, so the
  table reports `TESSERA_BOUNDARY_LOOP` once per failure episode.

Schedule (clamped; `maxAutoRetries` is clamped to `kMaxAutoRetries`):

| Retry ordinal | Backoff | Cumulative |
|---|---|---|
| 1 | 250 ms | 250 ms |
| 2 | 500 ms | 750 ms |
| 3 | 1000 ms | 1750 ms |
| 4 | 2000 ms | 3750 ms |
| 5 | 4000 ms (cap) | 7750 ms |

**Budget reading.** "At most five retries" counts retries, not executions: the initial attempt
plus five retries runs the guarded work at most six times. That is the only reading that reaches
the decided 4 s cap -- a four-retry budget stops at 2 s. The schedule is published once
(`backoff_seconds`) so the header, the tests, and the docs cannot drift; the values are binary
exact (`ldexp`) and tests compare them with `==`.

Anti-spin rules:

- `on_failure` while a retry is still `Waiting` consumes another retry, so a caller that keeps
  reporting failures cannot loop without spending the budget.
- A fallback failure calls `policy.reset()` and never auto-retries: re-running the fallback that
  just failed is a loop by construction.
- An async value never consumes the budget: auto-retry re-runs generated component code, and there
  is no inline async re-run to schedule.
- Reset (explicit or a `resetKey` change) clears the budget and the loop flag; only then can a
  boundary that hit the loop guard retry again.
- Time is never read here. The pump is `BoundaryTable::advance(id, now_seconds)`, called from the
  fixed-step loop; a retry becomes ready exactly at its deadline.

## 6. The cleanup seam

`SubtreeOwner::dispose_subtree(std::string_view)` is the single seam through which the boundary
releases a subtree. The runtime that owns scene nodes, tasks, and animations implements it;
`tessera_boundary` never touches scene state directly. `dispose_subtree` must not throw -- it runs
while the boundary is already handling a failure.

Dispose points:

| Trigger | Disposed subtree |
|---|---|
| catch (`show_fallback`) | the failed child subtree, exactly once (`child_detached` guards repeats) |
| retry ready (`advance`) | the live fallback, then the child is rebuilt by the caller |
| reset | the live fallback (the caller rebuilds the child) |
| fallback failure | the fallback subtree (the boundary stays in `Fallback` with nothing live) |
| unmount (`remove`) | the live fallback and, unless already detached, the child |

Ordering on a catch is deliberate: the child is detached and released **before** the fallback is
marked live and the report is delivered, so an onError that inspects the tree cannot observe a
half-disposed subtree. Two booleans (`child_detached`, `fallback_live`) make every dispose
idempotent; a double dispose is a use-after-free in the runtime, so the model refuses to emit one.

Unmount removes the boundary and every descendant from the registry after disposing them, so no
retry pump and no onError can address a removed id afterwards.

## 7. What is deliberately not caught

| Not caught by a component boundary | Why |
|---|---|
| `Fatal` (`bad_alloc`, stack overflow, abort) | A fallback cannot fix address-space exhaustion; the process handler/restart owns it. |
| `Platform` / GPU device loss | Recovery is a HAL/runtime concern; a UI fallback would hide a broken device. |
| Failures in the boundary's own fallback or onError | Converts to `FALLBACK_FAILED` at the parent/root; catching them again is a loop. A throwing onError is contained by `deliver` and escalated, never propagated. |
| Event throws with `catchEvents = false` | Strict-debugging switch: still contained at the root, still never thrown across a thread or FFI edge. |
| Cancellation as an error | A control outcome: no fallback, no onError, no state change. It is surfaced as a notification (section 8), not isolated. |
| Exceptions on worker threads or across C ABI/JNI/ObjC/Wasm | Forbidden outright; those edges convert to an `Error` value (`capture_current_error`) and post it to the UI thread. |
| Anything after unmount | No callback after teardown; stale async values are dropped with a diagnostic. |

The rule behind the table is "no silent catch": isolation is a decision about *who* handles a
failure, never about making it disappear. Even the one non-error path (cancellation) leaves a trace
-- a root notification and a counter.

## 8. Cancellation reconciliation

The two cancellation paths are intentionally asymmetric:

| Path | Entry point | Fallback | onError | Root handler | Counter |
|---|---|---|---|---|---|
| Async value | `report_async` | no | no | no | no |
| Caught exception | `catch_error` (via `run_guarded`) | no | no | one `CANCELLED` notification | yes |

Why: a cancellation value is produced *by* the owning task, so reporting it back to the same owner
would be a signal loop; the owner already knows. A cancellation thrown out of generated component
code is different -- it is control flow crossing the catch point, and AGENTS.md non-negotiable 16
puts cancellation in the root handler's inbox. Making it invisible would mean the app could cancel
a subtree and never learn that the cancel reached a guard instead of returning normally. The
notification uses `ErrorClass::Cancellation` and `TESSERA_BOUNDARY_CANCELLED`, so a handler can
separate it from errors without string matching, and
`BoundaryTable::cancellation_notifications()` is a monotonic lifetime counter ready to be scraped
by `diagnostics::MetricsRegistry` when the root boundary is wired.

Neither path shows a fallback, calls onError, stores an error in `BoundaryState`, detaches a
subtree, or consumes a retry. That is what "cancellation is not an error" means in code; the
notification is the one visible trace.

## 9. Diagnostics and privacy

Codes, sinks, and redaction rules are normative in spec section 7. Mechanically:

- A report is built once (`make_report`), redacted (`redact_paths`), and bounded before it leaves
  the boundary, so neither onError nor a crash breadcrumb sees a raw home path or unbounded text.
- Boundaries without onError report to the diagnostics sink; the sink is bounded and counts its
  own drops, so diagnostics can never grow without bound.
- The cancellation notification goes to the root handler only -- it is not an error report and does
  not call onError -- and is counted on the table.

## 10. Test map

Tests are deterministic: no wall clock, no threads, no RNG, injected fixed-step time, one component
per file. CTest names start with `boundary` so the lane can run `ctest -R boundary`.

| CTest name | File | What it pins |
|---|---|---|
| `boundary_model` | `boundary_model_tests.cpp` | registration errors, fallback plus exactly one report, boundary-in-Fallback escalation, fatal/platform bypass, `catchEvents`, reset/no-op reset, redaction and message caps, unmount cleanup |
| `boundary_retry` | `boundary_retry_tests.cpp` | exact 250/500/1000/2000/4000 ms schedule, clamp to five retries, readiness exactly at the deadline, loop guard reported once, reset restores the budget, table-level pump |
| `boundary_nested` | `boundary_nested_tests.cpp` | nearest catcher, promoted catch after fallback, fallback failure to parent/root, async fallback without retry, stale-generation drop, silent async cancellation, parent unmount |
| `boundary_guard` | `boundary_guard_tests.cpp` | guard success/throw, source span, unknown/OOM/platform classification, `catchEvents=false` escalation, FFI conversion, a throwing onError contained as `FALLBACK_FAILED` |
| `boundary_cancellation` | `boundary_cancellation_tests.cpp` | exception-path notification exactly once with the non-error class, per-catch counter, silent value path, no cleanup/retry/state change, cancellation while the fallback is live, unmounted source, counter survives reset |

Run: `ctest --test-dir build/lane-boundary-2 -R boundary --output-on-failure`.

## 11. Known limits and next seams

- Codegen does not emit guard calls yet; the IR/codegen integration is the next slice.
- The root boundary UI (what the root handler actually shows) and crash-report wiring are pending.
- The counter is not scraped by a live `MetricsRegistry` yet; the root boundary owns that wiring.
- The retry schedule is fixed, not configurable per boundary (decided for V1).
- Boundary-aware async ownership (which task belongs to which subtree/generation) is still an API
  seam; today the caller supplies the generation.
- A no-exceptions build would need explicit `Error` results; V1 generated code assumes exceptions
  are enabled.
