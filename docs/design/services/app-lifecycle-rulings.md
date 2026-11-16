# Application lifecycle and the AF3 rulings

Part of the [services design set](README.md). Section 11 introduces the AF3 slice and 11.1-11.3
record its two rulings and the ordered dispatch. 11.4-11.6 were filed under the same section in
the original document; they now live in [`background-and-lifecycle.md`](background-and-lifecycle.md)
and [`service-injection.md`](service-injection.md).

## 11. App lifecycle and the AF3 rulings

AF3 (issue #175) implements ADR-050 section 2: `appfw::Application`, the seven-state machine, ordered
lifecycle dispatch to services, the termination gate with `Defer`, and the `WindowHost` seam. Two
questions AF1 deferred are decided here, because the answers are properties of this slice.

### 11.1 Who marks the in-frame lookup region: the appfw tick wrapper

**Decision: `appfw::Application`'s tick wrapper marks the update/render stage; `RuntimeLoop` is left
untouched.** `RuntimeLoop::tick()` drains the completion queue first and only then advances the fixed
step and reports what to render (see `src/runtime/runtime_loop.cpp`), so the drain - the one place a
service's legal `start()`/`stop()` lookup can resume - is inside the same call as the update. A
`FrameScope` spanning the whole tick therefore marks the drain too and would count a service's own
`start()` resolution as a violation, which is a false positive on a contract the ADR explicitly
permits. Marking only the stage after the drain is not expressible from outside `RuntimeLoop::tick()`,
and splitting the method to expose the seam would put an app-framework concept (a lookup region) into
layer 0, which ADR-050 section 1 rule 1 forbids.

The wrapper is the app-framework tick boundary: the host that owns the frame step opens a
`FrameScope` around its update/render work (`begin_frame` through `end_frame`), never around
`RuntimeLoop::tick()` and never around the completion drain. `FrameScope` already counts nesting, so
a caller that wraps its own subsystem does not cancel the outer mark. The audit counter is therefore
authoritative for the update/render window the ADR names, and the drain stays legal.

**What AF3 actually wires, stated precisely.** AF3 records the decision and provides the machinery;
it does not yet open a `FrameScope` inside `WindowHost`. Nothing resolves a service while a frame runs
today: component `mount()` lookups arrive with AF5's codegen, and AF3's own service callbacks are
lifecycle notifications, not frame work. Wiring a mark with no caller would be dead code that the
tests cannot distinguish from a working one, so the mark is placed by the host slice that first has
in-frame lookups to catch, and `docs/reference/open-questions.md` records this entry as resolved with
that scope. What AF3 does guarantee is the other half: the dispatch path (`Application::notify_observers`)
runs outside any frame region, so a service reading a cached pointer in `on_app_state` can never be
miscounted as a violation.

Consequence for the acceptance evidence: the zero-violation claim is scoped to "no lookup while a
`FrameScope` is open", which is what AF1's tests assert. A lookup made from `on_app_state` is *outside*
that region (dispatch is not update/render work), so AF3 does not mark dispatch either: the ADR's rule
is about the frame path, not the lifecycle callback.

### 11.2 Does `start()` get a deadline: no

**Decision: `start()` keeps no deadline in V1.** ADR-050 section 4 item 4 pins a budget for shutdown
only (500 ms per service inside 2 s total), and says nothing about start. `ServiceRunnerConfig` names
the start deadlines separately (`per_service_start_deadline_seconds`,
`total_start_deadline_seconds`) with `0` meaning disabled, so the mechanism exists and is tested;
enabling it for production needs a line in ADR-050 section 4 item 4 that states the two numbers and
what a start that overruns should become (`Failed` today, by analogy with a failed start, but the ADR
does not say). Deciding that here would invent a startup budget for every app in the repository.

What AF3 does instead, so a start that never finishes cannot hang the lifecycle forever: `launch()`
and `tick()` drive `ServiceRunner::tick()` (which enforces whatever deadlines are configured), and the
app stays `Launching` until the start sequence leaves `Starting`. A host that wants a bound configures
the two fields; the framework does not silently pick one.

### 11.3 The ordered dispatch, and how existing signals route into it

The lifecycle is not a second mechanism beside the driver's occlusion pause; it is a layer above it.
`WindowHost` already folds normalized `hal::WindowEvent` values into `lifecycle_paused_` /
`metrics_paused_` (src/app/window_lifecycle.cpp) and honors `WindowPausePolicy`. AF3 adds one borrowed
`app::LifecycleSink*` to that fold (src/app/window_lifecycle_seam.cpp) and forwards the transitions it
already sees:

| Driver source | Forwarded as | Reaches |
|---|---|---|
| `WindowEventType::Focus` / `Blur` | `focus` / `blur` | `Application::dispatch` |
| `WindowEventType::Minimized` / `Occluded` | `minimize` / `occlude` | `Application::dispatch` |
| `WindowEventType::Restored` (with the live metrics) | `restore` | `apply_window_snapshot` |
| `WindowEventType::Close` | `close` | `dispatch` -> termination gate |
| `WindowMetrics` `occluded`/`minimized`/`focused` moving | the matching event, once per transition | `dispatch` |

Pause behaviour is unchanged: focus/blur never pause (a visible background window keeps painting), the
resume edge still latches a full repaint, and a window with no sink installed behaves exactly as
before. `idle_policy.hpp`'s pacing values are not lifecycle input and are not routed: they schedule
frames, they do not change app state.

Order is the ADR's, and it is what the tests assert as a vector:

1. **Forward transitions** (launch to `Active`/`Inactive`/`Hidden`, and `Resume`) dispatch in the
   runner's start order, so a dependency observes the new state before the dependents that read it.
2. **Graceful-stop transitions** (`Suspended`, and `Terminating`) dispatch in reverse start order,
   mirroring ADR-050 section 4 item 1's reverse `stop()`, so a dependent suspends before the
   dependency it uses.
3. A service observes transition *N* completely before any service observes *N+1*: dispatch is
   synchronous and one transition per call, never interleaved.
4. Only a service that reached `Started` is told; one whose start failed has nothing to react to,
   which also matches AF1's rule that only a `Started` service is stopped.

The termination gate runs the same reverse order: `on_terminate_requested()` is asked in reverse start
order, one `Defer` aborts the sequence and leaves everything running (`deferring()` reports it), and an
all-`Allow` sequence enters `Terminating`, notifies in reverse order, and begins AF1's bounded reverse
stop. An exception from a lifecycle callback is caught at the boundary (ADR-050 section 4 item 5): the
transition stands and the remaining observers are still called.
