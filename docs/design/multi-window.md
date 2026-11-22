# Multi-window: one device, one loop, many windows

Status: implemented (issue #228). This note records the ownership split the refactor chose, why the
per-device caches moved where they did, and what it deliberately leaves for later slices. The
prerequisite decision is [ADR-050](../../decisions/ADR-050/README.md) answer 3 ("multi-window: yes on
desktop in V1, as its own slice after one loop and one device per `Application` replace today's
per-window device, loop and blocking `run()`").

## 1. The problem this fixes

Before this slice, `app::WindowHost` created its own `rhi::Device`, its own `RuntimeLoop`, its own
`FramePacer` and its own copy of every per-device cache. Two windows therefore meant two devices, two
glyph atlases, two image caches, two pipeline sets and two recovery controllers — and `run()` was a
blocking loop per window, so a second window could not exist at all without a second thread.

Every per-device cache added before this refactor made it more expensive to fix: the glyph atlas, the
image and pipeline caches, `DeviceRecoveryController`, the pacer, and (from AF6) native surfaces.

## 2. The ownership split

The refactor introduces one owner above the windows and moves everything device-scoped into it.

| Object | Owner | Why |
|---|---|---|
| `rhi::Device` | **shared** (`WindowDevice`) | one GPU device per app; a second device duplicates every cache and every allocation |
| `DeviceRecoveryController` | **shared** | device loss is a property of the device, not of a window |
| `Batched2DRenderer` | **shared** | it owns the vertex ring, the offscreen pool and the glass pipelines, all device-scoped |
| `TextStack` (face, rasterizer, shaper, `TextRenderer`, glyph atlas) | **shared** | the atlas is the single largest per-device allocation, and a face is immutable |
| pipelines (main, present, LCD, glass blur/composite), white texture | **shared** | they are created against the swapchain *format*, which every window in a group shares |
| `RuntimeLoop` | **shared** | one tick drives every window; the loop is the app's clock, not a window's |
| `FramePacer` | **shared** | one present deadline for the app, so two windows cannot drift into two pacing regimes |
| `rhi::Swapchain` | **per window** | a swapchain is bound to one surface, and a surface to one window |
| `hal::Window` | **per window** | the platform window |
| `AppHost` (scene, style, layout, paint) | **per window** | each window has its own scene tree and its own layout |
| `ui::InputPump`, `FocusManager`, caret | **per window** | input and focus are per window by definition |
| `AccessibilityHost` | **per window** | a UIA/AT-SPI root is per window |
| `ScaledTargets`, `in_flight` command lists | **per window** | they are presentation-generation state |

The rule that decides every row: **if it is created against the device or the swapchain format, it is
shared; if it is created against a surface or a scene, it is per window.**

## 3. Why the renderer can be shared, and the one thing that had to change

`Batched2DRenderer` was already almost shareable: `begin()` takes the command list and the render
target as arguments, so it holds no reference to a swapchain. The one piece of per-frame state it
keeps is `frame_index_`, which it increments on every `begin()` and uses to index its vertex ring and
its glass target pool.

That ring is sized by `Batched2DRendererConfig::frames_in_flight`. With one renderer serving N
windows, a single tick can call `begin()` N times, so the ring must hold at least
`frames_in_flight * window_count` frames or a window would overwrite a ring slot the GPU is still
reading. The shared owner therefore sizes the config from the group's window count, and the
per-window `kFramesInFlight` slot gate stays exactly as it was — it is what proves a *window's* slot
is free, and it is independent of the ring's depth.

This is the one place the refactor changes a number rather than moving an object, so it is stated
explicitly: `frames_in_flight = kFramesInFlight * max_windows`, where `max_windows` is the group's
configured capacity (default 4, clamped).

## 4. Device loss fans out

A device loss is observed once, by the shared `DeviceRecoveryController`, and it invalidates every
window at once. The recovery sequence is therefore:

1. the loop's tick reports `device_blocked` / `device_failed` / `device_recovered` exactly as before;
2. the shared owner services the loss once (the in-place `recover()` attempt, then a replacement
   device if that is spent);
3. **every** window releases its presentation generation (swapchain, scaled targets, in-flight
   command lists) and recreates it against the new device;
4. a window whose recreation fails reports `stalled` and stops, without taking the other windows down.

Step 4 is the acceptance's "device loss recovers both" plus the failure isolation that makes it
useful: one window that cannot rebuild its swapchain must not end the app.

## 5. Resize and DPI are per window

`apply_metrics` stays on `WindowHost` and touches only that window: its swapchain is resized, its
`AppHost` re-lays out at its own logical size, and its own frame is requested. Nothing in the shared
owner is keyed on a window's extent, which is what makes "resize and DPI of one window do not relayout
the other" true by construction rather than by a check.

The one shared thing a resize can touch is the pacer, and only when the window's display refresh
changes: the pacer's divisor base follows the display the *focused* window is on. That is a deliberate
choice — one app presents at one rate — and it is recorded here because it is the only cross-window
coupling left.

## 6. What the public API looks like

`app::WindowHost` keeps its constructor and its whole method set, so every existing caller, test and
sample compiles unchanged. What is new is the owner:

- `app::WindowDevice` — the shared device, caches and loop. Constructed once per app, passed to each
  `WindowHost`.
- `WindowHost` gains a constructor overload taking a `WindowDevice&`; the old constructor creates a
  private one-window group, which is exactly the pre-refactor behaviour and keeps the single-window
  path byte-identical.

That overload is the compatibility seam: a one-window app links and behaves as before, and a
multi-window app constructs one `WindowDevice` and N hosts.

## 7. What is not here

- **Per-window `RuntimeLoop`.** One loop is the point; a window that wants its own tick rate is a
  later decision, not a missing feature.
- **Window groups with independent devices.** A second device is a second GPU context; nothing in V1
  needs one.
- **Tray apps** (#181) and **native surfaces** (AF6), both of which now have the shared device they
  need.

The `Application`-level window registry that used to be listed here landed in #180; section 10
records it.

## 8. Specs reconciled

The issue names five specs that still said "one window". Each is amended in this landing:

| Spec | What changed |
|---|---|
| `custom-elements-v1.md` Q9 | one device per app, not per element host |
| `runtime-config-v1.md` Q5 | the pacer is app-scoped; a per-window cap is not a V1 feature |
| `resource-lifecycle-v1.md` Q7 | device-scoped resources are released once, at the group's teardown |
| `overlays-and-layers-v1.md` | an overlay layer is per window; the device it draws through is shared |
| `startup-api-v1.md` section 10 | startup creates one device and then N windows |

## 9. Tests

- `app_window_multi` (Null RHI): two windows share one device, each presents, closing one leaves the
  other running, and a resize of one does not relayout the other.
- `app_window_multi_gpu` (lavapipe/real GPU): the same, with a real swapchain per window, plus a
  device-loss recovery that rebuilds both.
- `app_window_multi_loop` (real GPU, #180): one loop drives both windows, a change in one paints only
  that one, both present in the same group tick, and a settled window presents nothing.
- The existing single-window suites are unchanged and must stay green: the compatibility overload is
  what proves the refactor did not move the one-window path.

## 10. The window set (#180)

#228 made one device serve N windows but left the *loop* per window: a caller had to interleave two
blocking `run()` calls, which cannot work on one thread. #180 closes that, in two halves.

**Layer 1 owns the aggregate.** `appfw::WindowRegistry` (`include/tessera/appfw/window_registry.hpp`)
is a fixed-capacity table of `WindowLifecycle` values plus `aggregate_window_lifecycle`, the pure
function that folds them into the one aggregate `derive_app_state` already consumed. That is the
whole point: ADR-050 section 2 derives every state from a phrase about the *set* - "a window
focused", "all windows blurred", "no visible window" - so AF3's single `WindowLifecycle` member was
a one-window special case of the rule, not the rule. `Application` registers the primary window in
its constructor, so a single-window app's aggregate is exactly the member AF3 had and every
pre-#180 caller is unchanged; `attach_window()` adds the others and returns a `WindowId` that is
never reused.

The two rules the aggregation adds, both asserted row by row in `appfw_window_registry`:

- **A closed window is not visible**, even when its stale flags still say it is focused. A platform
  may report a focus for a window the app already closed.
- **Closing one of two windows is not a termination request.** `quitOnLastWindowClosed` is about the
  last *open* window, so the app stays `Active`/`Inactive` on the survivor. That is the acceptance's
  "independent lifecycle" in the form a user sees it.

**Layer 0 owns the walk.** `WindowDevice` keeps the live window list (`register_window` in a host's
constructor, `unregister_window` in its destructor) and `pump_windows_once()` gives every window one
bounded iteration. Two things had to move for that to be correct, and both were found by the
two-window pacing case rather than by reading the code:

1. **The tick is the group's.** `RuntimeLoop::tick()` *consumes* the render request, so a second
   window that ticked the loop itself saw `should_render == false` and never painted. The walk takes
   one tick and hands it to every window.
2. **The pacer advances once per tick, not once per window.** The pacer is app-scoped (section 5), so
   it gates *ticks*: advancing it per window let the first window in the walk consume the interval
   and starve its siblings for as long as the walk order stayed the same.

Both are the same mistake in two places - treating a per-tick resource as a per-window one - and
neither is visible without a real cap: an uncapped pacer has no interval, so `ready()` always says
"present now" and the bug is invisible. The test therefore asks for 30 Hz explicitly.

Related: [`app-host.md`](app-host.md), [`window-resize.md`](window-resize.md),
[`performance.md`](performance.md), [ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md),
[ADR-021](../../decisions/ADR-021-async-runtime.md),
[ADR-050](../../decisions/ADR-050/README.md).
