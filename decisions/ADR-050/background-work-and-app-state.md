# Background work and app-wide state

Part of the [ADR-050 decision set](README.md). Section bodies here: `### 5.` and `### 6.`. The heading
numbers, levels and order are the original ones, so `ADR-050 section N` still names the same
heading. Each part is a verbatim slice of the former single file; the shared front matter
(status, date, related decisions, terms) is in the [index](README.md).

### 5. Background work: task scopes, timers, desktop and mobile

1. **`TaskScope`** (layer 1, built on the existing `Task`, `CancellationSource` and
   `CompletionQueue`): `spawn(task)`, `cancel()`, `drained()`, a bounded task count, and a
   deadline-bounded `close()` used by the shutdown sequencer. Scopes nest: app, service, window,
   component (unmount cancels). This is how "no detached tasks" becomes enforceable: the framework
   offers no way to start a task without a scope.
2. **Timers** are a layer 0 wake source: `RuntimeLoop` gains a bounded timer queue whose earliest
   deadline joins the pacer deadline, so an idle app parks until the next timer instead of polling.
   One-shot and periodic; a periodic timer coalesces missed periods; timers are deterministic under
   `ManualClock`. A timer never forces a frame; its callback schedules work like any completion.
3. **Desktop.** `Hidden` stops rendering (the `runtime-config-v1.md` section 5.8 policy, which the
   window driver must start honoring) but services keep running under their
   `BackgroundPolicy { Continue, Throttle, Pause }` (default `Continue`).
4. **Mobile.** `Hidden` asks every service to checkpoint (`on_app_state`); `Suspended` pauses all
   timers and cancels scopes whose policy is not `Continue`; in-flight I/O may finish only within
   the platform grace period, then it is cancelled. No rendering and no GPU work while not visible.
5. **OS-scheduled work** (Android WorkManager/JobScheduler, iOS BGTaskScheduler, Android
   foreground services) runs outside the visible app lifetime. The proposed contract is a
   **headless entry**: the same `Application` started without windows, with a declared subset of
   services, under the OS time budget. Scope and timing are open (question 1).

### 6. App-wide state

Options: (a) component-local state and props only (status quo); (b) context propagation through the
tree; (c) observable values owned by services; (d) a reducer/action store (Redux style); (e) a
signals graph with automatic dependency tracking.

**Decision: (a) stays the default for component state; app-wide state is (c), owned by services.**

- `Observable<T>` (layer 1) holds a value and a version on the UI thread. `subscribe(UpdateTicket&)`
  returns an RAII `Subscription`; `set()` compares with `operator==` and, on change, calls
  `schedule()` on each subscribed ticket. Updates reuse the existing `UpdateScheduler` batching:
  no synchronous re-render, one flush per tick, per-window schedulers work unchanged.
- A **store** is a service that owns observables and the methods that change them. Writes happen on
  the UI thread; background results arrive through the completion queue and then call `set()`.
- Subscriber storage is allocated at `subscribe()` (mount), never at `set()`; notifying is a loop
  over intrusive links.
- `NavController`, the async-data `DataStore` and `PermissionService` expose their state as
  observables, so every screen subscribes the same way.
- There is no `useStore` hook. In V1 a developer class subscribes in C++ and mirrors values into
  state cells with the ADR-040 setters; TSX renders those cells. Whether a manifest-listed
  observable on a service interface may be read directly in TSX render (lowered to a mount-time
  subscription) is open question 6.
- Rejected as the mandated pattern: (b) for the component-model reasons in section 3; (d) because
  reducers need switch-heavy code and an action log the V1 surface rejects (`useReducer` is unsafe
  in [`react-compat.md`](../../docs/specs/jsx-intrinsics-v1/react-compat.md)), though an app may build
  one on top of (c); (e) because automatic tracking hides allocations and ordering. Derived values
  (`Computed<T>`) are deferred.
