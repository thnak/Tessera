# ADR-017: Input handler and fixed-step runtime loop

- Status: Proposed
- Date: 2026-11-07
- Related: [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin render hardware interface and batched 2D renderer), [ADR-009](ADR-009-deterministic-headless-golden-tests.md) (deterministic headless golden tests), [ADR-016](ADR-016-stylesheet-engine.md) (stylesheet engine), [ADR-018](ADR-018-dirty-rendering-and-frame-pacing.md) (dirty rendering and frame pacing)

## Context

Tessera has no browser event loop or DOM. Native input must be normalized, hit-tested against
the [scene graph](../docs/reference/glossary.md), and propagated without platform types leaking into UI code. Charts, animations,
and deterministic [goldens](../docs/reference/glossary.md) require a game-engine-style clock rather than a variable wall-clock
delta. The runtime must also support [render-on-demand](../docs/reference/glossary.md) for static tool UIs and continuous
presentation for dashboards/video.

## Decision

1. `hal/input` normalizes pointer/mouse/touch/keyboard/text/IME events from platform backends
   into one `InputEvent` queue. No platform type crosses into `ui/`.
2. The runtime owns a **fixed timestep** simulation at `fixedTimestepHz` (default 60), with an
   accumulator, `maxFrameDeltaMs`, `maxUpdatesPerFrame`, time scale, and pause (`timeScale=0`).
3. Input is sampled **once per simulation tick** (one fixed-timestep update); render may run at a different rate and
   interpolates visual state.
4. Event flow is W3C-style: hit test -> capture -> target -> bubble. `stop_propagation` and
   `prevent_default` are separate.
5. The input subsystem maintains focus, hover, active, and pointer-capture state, and drives
   the stylesheet pseudo-class dispatcher (`:hover`, `:active`, `:focus`, `:focus-visible`,
   `:disabled`, `:checked`) before style resolution.
6. Deterministic mode uses a simulated clock, a recorded input script, fixed viewport/DPI, and
   exact tick counts. The golden/headless runner steps N ticks and captures frames.
7. [V1](../docs/reference/glossary.md) is UI-thread-only. Platform callbacks push into a bounded SPSC (single-producer single-consumer) queue; the UI thread owns
   dispatch. No locks are held while calling handlers.
8. Render-on-demand wake sources: input, dirty state, active animations/timers, external data,
   resize/DPI/theme, explicit `request_frame()`.
9. Metrics expose queue depth, hit tests, focus changes, input-to-style/render latency, steps,
   and dropped steps.

## Consequences

### Positive

- Deterministic simulation and goldens independent of wall-clock or display rate.
- One normalized event model for all platforms.
- Pseudo-classes and dirty style invalidation work consistently.
- Static UIs can idle at near-zero CPU/GPU usage.

### Negative / risks

- Fixed-step catch-up needs careful clamping; a debugger pause must not freeze the app.
- Input coalescing and pointer capture have subtle multi-touch edge cases.
- IME (input method editor) and accessibility actions are platform-heavy and are not in the first implementation step; the Follow-up list places them after the platform backends.
- Render-on-demand interactions with VRR (variable refresh rate) and [frame pacing](../docs/reference/glossary.md) need tuning ([ADR-018](ADR-018-dirty-rendering-and-frame-pacing.md)).

## Alternatives considered

- **Variable timestep / browser rAF-style delta.** Rejected: non-deterministic, physics/animation
  drift, poor goldens.
- **Event-driven loop only.** Rejected: real-time charts/animations need a clock; input-only wake
  is insufficient.
- **Dispatch input on a render thread.** Rejected for V1: scene graph and style state are
  UI-thread-owned; latency/complexity not justified for V1.
- **Browser DOM event model.** Rejected: no browser in production.

## Follow-up

- Implement the input queue, hit-test dispatch, focus/pointer capture, and pseudo-state updates.
- Implement the fixed-step loop and deterministic runner.
- Add platform backends (Win32, X11, Wayland), then IME/text input.
- Add input recording/replay and latency metrics.
