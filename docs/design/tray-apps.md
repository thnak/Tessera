# Tray apps: a window-less app that still runs

Status: implemented (issue #181). This note records what a tray-resident desktop app needs from the
engine, why the answer is two small seams rather than a tray API, and what is deliberately left to
the `shell` module. The prerequisite decisions are [ADR-050](../../decisions/ADR-050/README.md)
answer 1 ("tray-resident desktop apps in V1 (`Hidden` with zero windows)") and
[`platform-shell-v1.md`](../specs/platform-shell-v1.md) section 1, which marks the tray/status item
as V1 on Windows and macOS.

## 1. The problem this fixes

A tray app is an app whose steady state is "running, with no window on screen". The user sees a
status item, not a window; clicking it opens one. Everything the app does between clicks - a sync, a
timer, a network response - has to keep working while nothing is painted.

Two things were missing, and they are in different layers.

**Layer 1 already had the state.** #180 landed `appfw::WindowRegistry`, whose aggregation rule says
an empty window set is `closed`, which `derive_app_state` maps to `Hidden`. With
`quitOnLastWindowClosed = false` that is a running app with no window, and #180's
`test_tray_app_stays_alive_with_no_window` asserts exactly that row. So the *lifecycle* half of a
tray app was already correct.

**Layer 0 had no loop for it.** The only loop was `WindowHost::run()`, and it parks on *its own*
window's event wait (`hal::wait_for_events_until`). An app with no window therefore had no loop at
all: there was nothing to park on and nothing to tick. A tray app could only spin.

**And a hidden window still painted.** `WindowOptions::visible = false` created a window the user
never sees, but the driver still acquired, painted and presented every frame - the smoke test
asserted `presented >= 3` on exactly that window. That is correct for its actual purpose (the
offscreen/CI pattern, where the app paints into an invisible window on purpose) and wrong for a tray
app, which must cost no GPU work while hidden.

## 2. The two seams

### 2.1 `WindowHost::set_visible` - hiding is the app's decision

`set_visible(false)` hides the platform window and stops the driver presenting; `set_visible(true)`
shows it and latches a full repaint. The state is a driver flag (`hidden_by_app_`), not a platform
query, and that is the whole design decision:

| Creation state | Runtime hide |
|---|---|
| `WindowOptions::visible = false` | `WindowHost::set_visible(false)` |
| the offscreen/CI pattern: paint into a window nobody sees | the tray app: paint nothing |
| **keeps presenting** | **presents nothing** |
| a capture, a bounded CI run, a golden | a status-item app between activations |

Conflating the two would break one of them. Making `visible = false` stop presenting hangs every
bounded `run(N)` on an offscreen window - twelve GPU tests and two samples do exactly that, and the
first version of this slice did hang them. Making `set_visible(false)` keep presenting leaves the
tray app burning a GPU on a scene nobody can see.

The pause is deliberately **not** routed through `WindowPausePolicy`. That policy is the
`pauseWhenOccluded` config switch, and a config switch must not be able to make a hidden window
paint: a hidden window has no target to present into, so "keep rendering" is not an option a caller
can ask for. `paused()` is therefore `paused_ || hidden_by_app_`, and the two sources stay separate.

Showing latches a full repaint because nothing painted while hidden describes the screen. The
request also marks the loop dirty, which is what wakes an on-demand loop that has been parked since
the hide - without it, activation would show a window and then wait for an unrelated dirty source.

### 2.2 `WindowDevice::run_windows` - the group's own loop

`run_windows(max_frames)` is the loop a window-less app runs. It pumps every registered window (via
`pump_windows_once`, so a tray app that *does* have a window uses the same path), then parks for
`next_wait_seconds()`.

`next_wait_seconds()` is the piece that makes a window-less loop possible: it is the nearer of the
pacer's present deadline and the next timer, clamped to `kMaxIdleWaitSeconds` when neither schedules
anything. With no window there is no platform event wait to join, so the park is a plain bounded
sleep - which is exactly what a tray app does between activations. The clamp is what keeps it from
sleeping forever: an uncapped pacer has no future deadline and a loop with no timer has none either.

`request_stop()` ends the run at its next iteration boundary. It is the only way a window-less run
can end, because there is no window whose close would stop it. The request is *consumed* rather than
merely observed, so a second run is not stopped by the first one's request - the same re-entrancy
rule `run()` follows.

A bounded run (`max_frames > 0`) on a group with no window returns at once with `presented == 0`
rather than parking for frames that cannot happen: a caller that asked for frames learns there is
nothing to paint.

## 3. What the app does

```cpp
appfw::ApplicationConfig config;
config.quit_on_last_window_closed = false;   // closing the window is not quitting
appfw::Application app(runner, config);

app::WindowDevice group;                     // one device, one loop, zero windows
// ... services start, timers are scheduled, the tray item is registered by the shell module ...

group.run_windows();                         // parks; paints nothing; services keep running

// On activation, from the tray item's click action:
auto root = build_main_window();
app::WindowHost window(*root, sheet, options, group);
group.pump_windows_once();                   // paints the window that just appeared
```

The two halves meet at the aggregate: the window's lifecycle events reach `Application` through
`make_lifecycle_sink(WindowId)`, so the app state moves `Hidden -> Active` on activation and back on
hide, and services observe both transitions in start order.

## 4. What is not here

- **The tray item itself.** `platform-shell-v1.md` section 6 owns the model (icon, tooltip, menu,
  click action) and the `shell` module owns the capability bits; `ShellCapability::Tray` already
  exists. This slice is the lifecycle and loop half, which is what ADR-050 answer 1 asked for.
- **Linux tray.** `platform-shell-v1.md` section 10 lists the Linux tray/status-item matrix as an
  open question, and section 9 records Linux tray as best-effort and not in V1.
- **A window-less `Application` start.** ADR-050 answer 1 names this as a seam. It is not needed for
  the tray case - the app starts with a window and hides it, or starts with none and attaches one -
  and P1-P5 are AF5's, so it stays recorded rather than guessed at.
- **`BackgroundTask`.** The other seam answer 1 names, for OS-scheduled work. That is Big Step F
  (mobile platform back ends), and there is nothing to test it against yet.

## 5. Tests

| Test | What it proves |
|---|---|
| `app_window_tray` (GPU, `tests/gpu/app_window_tray_tests.cpp`) | a hidden window presents nothing across several iterations and after damage; an offscreen-created window still presents; showing paints and the first frame is a full repaint; hiding again stops presenting and keeps the window in the group; a window-less group reports a wait and a bounded run returns at once; `request_stop()` ends an unbounded run; a tray app activates into a window and paints it |
| `appfw_lifecycle` (`tests/unit/appfw_window_registry_tests.cpp`) | the layer-1 half, landed in #180: an empty window set is `closed`, derives `Hidden`, and a focus brings the app back |

The GPU test is the acceptance because "zero presents" is a claim about the present path: a lane
with no presentation surface prints `skipped`, which CTest turns into a skip rather than a pass. The
test was verified to fail with the fix reverted (five assertions), so it is not a test that passes
for the wrong reason.
