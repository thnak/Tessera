# Activity tracking and idle deadlines

- Status: proposed. No code implements this yet; every "today" statement below names the file it was
  read from.
- Related: [ADR-019](../../decisions/ADR-019-power-aware-runtime.md) (power-aware defaults; static
  scenes idle), [ADR-017](../../decisions/ADR-017-input-and-fixed-step-runtime.md) (input and
  fixed-step runtime), [ADR-050](../../decisions/ADR-050/README.md) (app framework layering),
  [ADR-021](../../decisions/ADR-021-async-runtime.md) (never block the UI thread),
  [`background-work.md`](background-work.md) (the timer wake source),
  [`game-loop.md`](game-loop.md) (fixed timestep, idle policy),
  [`multi-window.md`](multi-window.md) (one loop, many windows),
  [`app-host.md`](app-host.md) (steps 3-8 and what the driver owns).

Terms: an **activity edge** is any input the app observes; an **idle deadline** is an absolute
instant on the app's clock at which "no activity for N seconds" becomes true; **park** is the
bounded platform wait between ticks.

## 1. What the question is

Several features want the same fact — "the user has not touched anything for a while": an idle
logout, an auto-save, a kiosk attract mode, a screensaver, an ambient overlay. None of them is
expressible today, because there is no notion of user inactivity anywhere in the tree.

This note specifies the one seam they should share and nowhere duplicates: a per-window activity
tracker plus a deadline that the runtime loop can park against. It deliberately does not specify
what any feature *does* when the deadline fires.

## 2. What exists today, precisely

| Piece | State | Where |
|---|---|---|
| Input observation before routing | one slot, `InputPump::set_input_filter` | `include/tessera/ui/input_pump.hpp` |
| Who owns that slot | `WindowHost` (overlay dismissal) and `TestApp` | `src/app/window.cpp`, `src/testing/test_app.cpp` |
| Timestamp on every event | `InputEvent::timestamp_s`, filled by the producer | `include/tessera/ui/input.hpp` |
| Wake source queue | `runtime::TimerQueue`, fixed capacity 64, one-shot and periodic | `include/tessera/runtime/timer_queue.hpp` |
| Who fires it | `RuntimeLoop::tick()`, after the completion drain and before the fixed step | `src/runtime/runtime_loop.cpp` |
| Who attaches one | `TestApp` only (`loop_.set_timer_queue(&timers_)`) | `src/testing/test_app.cpp` |
| Loop clock | `SteadyClock` in the app, `ManualClock` in tests | `include/tessera/runtime/steady_clock.hpp` |
| Idle park budget | `kMaxIdleWaitSeconds = 0.25`, plus the pacer join | `include/tessera/app/idle_policy.hpp` |
| Group-level join of pacer and timer | `WindowDevice::next_wait_seconds()` | `src/app/window_device.cpp` |

Two facts decide the whole design:

1. **There is no production `TimerQueue`.** `WindowDevice` constructs its `RuntimeLoop` with no timer
   queue (`src/app/window_device.cpp`), so `next_timer_wait_seconds()` returns `kNoDeadline` and the
   group's park is the pacer deadline or the 250 ms clamp. `appfw::NetClient` takes a `TimerQueue&`
   for its request deadline, but nothing in the app layer creates one to pass. Any idle deadline
   therefore needs the queue to be *owned* before it can be scheduled.
2. **A timer is a wake source, not a render cause.** `RuntimeLoop::tick()` fires due timers and
   counts them in `RuntimeTick::timers_fired`, and it explicitly does not set `frame_requested_`
   (ADR-050 section 5 item 2). So a deadline firing does not paint anything by itself; the callback
   must ask for what it wants.

## 3. Ownership

Three pieces, each in the layer that owns its concept (ADR-050 section 1 rule 1: layer 0 gains
seams, not concepts).

| Piece | Layer | Why there |
|---|---|---|
| `runtime::TimerQueue` instance | `WindowDevice` | it is the loop's wake source, and the loop already lives there |
| `app::ActivityTracker` | `tessera_app` | "the user is idle" is an app concept, like `MotionPreference` in `system_motion.hpp` |
| Consumer policy (logout, attract mode) | the app | the tracker reports; it never acts |

`ActivityTracker` is a plain value type: one `double last_activity_seconds`, a configurable timeout,
and a `poll(now)` that returns whether the edge was crossed. It holds no timer, no callback and no
sink, so it is testable with `ManualClock` and costs one subtraction per tick.

## 4. The activity edge

`InputPump::set_input_filter` is the only place that sees every event before the router, and it has
**one slot**. `WindowHost` already occupies it:

```cpp
// src/app/window.cpp — today
pump.set_input_filter(
    [](void* user, ui::Node&, const ui::InputEvent& event) {
        return static_cast<AppHost*>(user)->handle_overlay_input(event);
    },
    host.get());
```

The slot must become a chain, not be taken twice. Three options, in order of preference:

1. **Chain the filters.** Give `InputPump` a second, separately-installed observer that cannot
   consume (`set_input_observer(Notifier, void*)`, returning `void`). Overlay dismissal keeps
   consume authority; the observer only records. This is the smallest change and leaves the
   dismissal rules of [`overlays-and-layers-v1.md`](../specs/overlays-and-layers-v1.md) untouched.
2. Chain inside `AppHost`: `handle_overlay_input` records activity before delegating. Rejected as the
   primary design because it puts a UI concern in the overlay path and makes the tracker invisible
   to a host that installs its own filter.
3. A second slot on `InputPump`. Rejected: two filters with different consume semantics is exactly
   the ambiguity the single slot avoids.

**Which event types reset the edge.** All of them, not only pointer input. The list is the
`InputEventType` enumerator set in `include/tessera/ui/input.hpp`: `PointerMove`, `PointerDown`,
`PointerUp`, `PointerCancel`, `Wheel`, `KeyDown`, `KeyUp`, `Text`, `CompositionStart/Update/End`,
`Focus`, `Blur`, `EditText`, `BackRequested`, `VolumeKey`. A hover-driven octopus that keeps moving
because the user is typing is the bug this rule prevents. `PointerMove` with zero delta is the one
exception worth naming: some platforms emit synthetic moves on a stationary pointer; a move whose
`dx` and `dy` are both zero does not reset the edge.

**What the app cannot see.** Input delivered to another application, a session lock, a sleeping
display, and a touchscreen used elsewhere are invisible to `InputPump`. "Screen still" therefore has
two different meanings, and the seam must let a consumer ask for the stronger one:

- *local inactivity* — no event reached this window. Works everywhere, needs no platform support.
- *system inactivity* — the OS's own last-input time. Windows has `GetLastInputInfo`, X11 has
  `XScreenSaverQueryInfo`; Wayland has no equivalent for a normal client, and Android has none.

The seam follows the precedent of `app::system_prefers_reduced_motion()` in
`include/tessera/app/system_motion.hpp`, which already models "a platform that cannot answer reports
false, and the driver reads it once":

```cpp
// include/tessera/app/system_idle.hpp — proposed
struct SystemIdle {
    double seconds = 0.0;
    bool available = false; // false: the platform has no query
};
// Never blocks. Windows: GetLastInputInfo; X11: XScreenSaverQueryInfo; Wayland and Android: no
// query, so `available == false` (macOS is post-V1, ADR-010).
[[nodiscard]] SystemIdle system_idle() noexcept;
```

It is proposed in `tessera_app` rather than `hal` for the same reason `system_prefers_reduced_motion`
is: the HAL abstracts windows and displays, and this is a process-wide fact with no window handle.

A consumer that asks for system idle on a platform without it must degrade to the local edge rather
than refuse to run. That fallback is the caller's, not the tracker's.

## 5. The deadline

The tracker answers "is the user idle now". A deadline still has to *wake* the app, because
ADR-019 decision 4 requires a settled scene to park with zero presents, and a parked app does not
poll.

```text
activity edge at t0
  -> activity.schedule(now)         // one-shot at now + timeout, on the WindowDevice timer queue
  -> RuntimeLoop parks on min(pacer deadline, next timer)
  -> timer fires inside tick()      // timers_fired += 1, no frame requested
  -> callback asks for what it wants (request_frame / open_overlay / mark_render_dirty)
```

Three rules follow from the existing machinery and must be stated normatively, because each one has
a wrong alternative that looks cheaper:

1. **Reschedule, never poll.** Every activity edge cancels and re-schedules one one-shot. A periodic
   timer ticking every second would wake the app 60 times per timeout; `TimerQueue` is fixed
   capacity and a periodic timer's late fire coalesces to one callback
   ([`background-work.md`](background-work.md) section 3.2), so a periodic reading is both wasteful
   and lossy at the edges.
2. **The deadline is absolute on the loop's clock**, computed from `Clock::now_seconds()`, never from
   wall time and never from `steady_now_seconds()` directly. That is what `TestApp`'s `ManualClock`
   drives, and it is what makes the feature golden-testable.
3. **The timer callback does not paint.** Per ADR-050 section 5 item 2 it schedules work; a consumer
   that wants pixels calls `request_frame()` or opens an overlay through
   `AppHost::open_overlay()`, which already calls `mark_render_dirty()`
   (`src/app/host.cpp`).

### 5.1 The park costs more than it looks

`WindowDevice::next_wait_seconds()` already joins the pacer deadline with
`loop_->next_timer_wait_seconds()` and clamps to `kMaxIdleWaitSeconds`. **`WindowHost::run()` does
not**: it parks on `idle_wait_seconds(pacer->ready(...))` from `idle_policy.hpp`, which knows only
the pacer (`src/app/window_run.cpp`). With no damage and no pacer deadline that returns the 250 ms
clamp, so the single-window loop ticks about four times a second forever.

That is acceptable today because a tick of a settled scene is cheap and paints nothing. It stops
being acceptable the moment a timer exists, because the timer would be noticed up to 250 ms late and
the app would tick four times a second to discover there is nothing to do. The single-window park
must therefore join the timer deadline exactly as the group park does. This is a small change to
`idle_policy.hpp` plus `window_run.cpp`, and it is a precondition of this design, not a follow-up.

## 6. Multi-window

One `WindowDevice` owns one loop, one clock and (proposed) one `TimerQueue`, and may drive N
windows (ADR-050 answer 3, [`multi-window.md`](multi-window.md)). Two consequences:

- **Deadlines are global, activity is per window.** The tracker lives on `WindowHost`; its deadline
  is scheduled on the shared queue. A single timer id per window is enough, and the queue's capacity
  (64) is not a constraint at realistic window counts.
- **The question "is the app idle" is an aggregation**, not a per-window flag: the app is idle when
  every visible window is idle. That rule belongs to the consumer; the tracker exposes per window
  and the consumer folds. A tray app with no window has no local edge at all and is a
  system-idle-only consumer.

## 7. Determinism and tests

Everything is already injectable, which is why this seam needs no test-only special case:

| Test | Mechanism |
|---|---|
| Edge resets on every event type | `TestApp::post_input` → pump → observer |
| A zero-delta pointer move does not reset | same, `dx = dy = 0` |
| Deadline fires exactly once at `t0 + timeout` | `ManualClock::advance` + `TimerQueue::fire_due` through `RuntimeLoop::tick()` |
| An edge before the deadline cancels the old fire | advance to `t0 + timeout - 1`, post input, advance past, assert no callback |
| A settled app with an armed deadline still parks | `RuntimeTick::idle` true and `timers_fired == 0` between edges |
| The single-window park honours the timer | `pump_once` count over a settled interval is 0 until the deadline |
| No wall clock | grep-level rule: the tracker and its consumers take `now` as a parameter |

The last row is a rule, not a test: `ActivityTracker::poll(now)` takes the instant, mirroring
`TimerQueue::fire_due(now)` and `InputPump::tick(timestamp_s)`. A type that reads a clock cannot be
driven by `ManualClock`.

## 8. Diagnostics

`FrameDiagnostics` gains two counters, both plain integers, no allocation:

- `idle_edges` — how many times the deadline fired since the host was created.
- `activity_resets` — how many armed deadlines a later event cancelled, which is the number that
  explains "the feature never triggers" on a machine with a jittery touchpad.

## 9. What this note does not decide

- **Whether an app may wake a settled scene at all.** ADR-019 decision 4 says a static scene presents
  nothing; a feature that paints on an idle deadline is a deliberate, bounded exception to it. That
  decision, and its bound (max duration, max wake frames, stop on any input, off under
  `powerPolicy=power-save` and under reduced motion), belongs to the consumer and is written down in
  [`decorative-animation.md`](decorative-animation.md), not here.
- **System idle on Wayland and Android.** No query exists; the local edge is the documented
  fallback.
- **Session lock and display sleep.** `os-events-v1.md` owns lifecycle events; a locked or sleeping
  session should reach the consumer as a lifecycle transition, not as inactivity.
- **Whether the tracker belongs to `AppHost` or to `WindowHost`.** The proposal is `WindowHost`
  because that is where the pump is, but a headless `TestApp`-only consumer would want it on the
  host. Either is compatible with sections 4-5; the choice is an implementation detail with one
  constraint: the queue it schedules on must outlive it.
