# Animation and transitions

Status: implemented B2 runtime slice for
[Big Step B2](../../AGENTS.md#big-steps-group-work-dont-micro-step); the overscroll interaction
design remains open. Runtime semantics are in
[`animation-runtime.md`](animation-runtime.md). Normative animation/interaction/overscroll
contract: [`animation-v1.md`](../specs/animation-v1.md).

Animation is declarative at the style/component level and sampled by the fixed-step runtime ([`game-loop.md`](game-loop.md)).
It never reads the wall clock and never blocks a frame.

## 1. Sources of animation

| Source | Example | Owner |
|---|---|---|
| CSS transitions | `transition: opacity 120ms ease-out` | style engine |
| CSS keyframes | `@keyframes pulse { ... }` | style engine |
| Animated values | `useAnimatedValue(0)` | component/runtime |
| Spring | `{ type: "spring", stiffness, damping }` | animation runtime |
| Interaction feedback | press/hover/focus/validation | component/library + runtime |
| Scroll effects | fling/snap/scroll-to/scrollbar fade | scroll controller |
| Overscroll | stretch/bounce/glow + spring-back | scroll + animation runtime |
| Route/overlay | enter/exit/focus | navigation/overlay |
| View transition | theme switch circle reveal, crossfade, shader glitch | animation runtime + compositor ([spec](../specs/view-transitions-v1.md)) |
| Scroll-driven | not specified (open question, section 11) | layout/scroll |

## 2. Timeline and sampling

```text
tick(dt):
  reject non-finite or negative dt (state unchanged)
  advance every unpaused entry by dt
  move entries that reached their end to the finished set
  clamp forwards/both entries to the final sample and retain them as fills
  remove finished entries from the active list
  run completion callbacks in finished-entry order
  sample active entries, then retained fills (post-callback)
  write samples into computed style
  mark layout/paint/render as needed
  keep awake only while unpaused entries remain
```

- `tick` is one update of the fixed-step runtime loop ([`game-loop.md`](game-loop.md)); elapsed time is accumulated from `dt`, never from wall time.
- `dt` is the fixed timestep (e.g. 16.666 ms); animation is frame-rate independent.
- Ordering is normative: callbacks run after fills are retained but before samples are produced, so a callback that clears, stops, restarts, or starts an entry is reflected in the same tick result.
- Deterministic mode (fixed clock, no wall time; see [ADR-017](../../decisions/ADR-017-input-and-fixed-step-runtime.md), input and fixed-step runtime) uses the same fixed clock and an injected start time.
- Sampling happens **before layout/paint** so animated values participate in the frame.

## 3. Easing and interpolation

V1 (first-release scope, see [AGENTS.md rule 8](../../AGENTS.md#non-negotiables)):

- linear, ease, ease-in, ease-out, ease-in-out
- cubic-bezier(x1,y1,x2,y2)
- steps(n, start|end)
- spring (damped harmonic oscillator; deterministic integration)

Interpolate:

- numbers, lengths, percentages (when compatible)
- colors in a documented space (sRGB in V1; wide-gamut has no decision yet, see section 11)
- transforms (translate/scale/rotate/skew) as affine matrices
- opacity

Discrete properties (display, visibility) use step semantics; do not interpolate them.

## 4. Transitions

- A transition starts when a computed property changes and a transition is declared.
- Interrupted transitions retarget from the current value; do not jump to the old endpoint.
- Multiple properties can run with different durations/delays/easings.
- `transition: none` cancels/does not start.
- Transitions run on the UI thread and mark only the affected dirty domains (style/layout/paint/render invalidation scopes, see [`dirty-rendering.md`](dirty-rendering.md)).

## 5. Keyframes

- `@keyframes` parsed by the style engine; sampling uses the fixed clock.
- `animation-iteration-count`, `direction`, `fill-mode`, `delay`, `duration`, `timing-function`.
- `infinite` animations are allowed but must respect power policy ([ADR-019](../../decisions/ADR-019-power-aware-runtime.md), power-aware runtime defaults): pause when hidden/occluded,
  and reduce/disable optional motion when `prefers-reduced-motion` is set.
- Animations that only affect paint (opacity/color) do not mark layout dirty.
- Under reduced motion, opacity-only keyframes clamp to **120 ms** with zero delay and one
  `normal`-direction iteration. Any keyframes containing a non-opacity property become instant
  (`duration = 0`); fill mode, play state, and easing are preserved.

## 5.1 Interaction and overscroll effects

Normative details: [`animation-v1.md`](../specs/animation-v1.md).

- Press/hover/focus/validation/list/route/overlay effects use the same fixed-step sampler.
- Scroll offset stays clamped; overscroll is a separate visual state (`visual`, `velocity`) with
  `None/Glow/Stretch/Bounce/Platform` modes.
- Dragging beyond an edge applies asymptotic resistance; release runs a damped spring back to
  zero. A new pointer down catches the current visual value without a jump.
- Stretch/bounce transforms the content layer only; sticky headers/scrollbars/overlays stay
  outside the effect. No relayout; damage (the screen region that must be redrawn) is the visual union.
- `prefers-reduced-motion` disables stretch/bounce/glow and clamps immediately.
- The effect never changes scroll offset, content size, focus, or automation IDs.

## 6. Animated values (`useAnimatedValue`)

- Creates a runtime value with an initial value and a driver (tween/spring).
- Generated C++ owns the `AnimationController`; the component reads the sampled value.
- Setting the value starts/retargets the animation; `set(value, { animate: false })` snaps.
- Animated values can drive style properties, transforms, or chart data.
- Implemented core: `ui::animation::AnimatedScalar` (`include/tessera/ui/animation/animated_scalar.hpp`). It is a
  clock-free scalar the owner ticks with the frame delta: `set(v, {duration, easing, animate})` retargets from the
  current value (no jump), defaults to CSS `ease-out` over 200 ms, snaps for `animate:false`, a zero duration or
  reduced motion, and `tick` returns whether another frame is needed. Not yet wired: the `AppHost` registry that
  ticks scalars and wakes the loop, the generated-component handle, and codegen for `AnimSet` (it still emits a
  plain assignment). Springs as a scalar driver also wait on `SpringRuntime` gaining an owner.
- Completion callbacks are delivered on the UI thread via the completion queue ([`async.md`](async.md)); no detached
  tasks.

## 7. Scheduling and power

- A frame with active animations is a wake source.
- No active animations -> no animation ticks; if nothing else is dirty, the loop idles.
- Hidden/occluded/minimized -> pause animations (unless the runtime config key `pauseWhenOccluded` is `false`; see [`runtime-config-v1.md`](../specs/runtime-config-v1.md)).
- `powerPolicy` (runtime setting, see [`runtime-config-v1.md`](../specs/runtime-config-v1.md)) may reduce animation quality (e.g. fewer samples, no spring overshoot) before
  dropping frames.
- `prefers-reduced-motion` -> skip non-essential motion, keep functional state changes. The
  runtime setter applies at start/retarget time; changing the preference does not retroactively
  rewrite running entries, so restart or re-sync when it changes. Springs snap to their target
  instead of integrating.
- Never busy-wait for the next animation sample; use the loop's deadline/wakeup.
- The runtime-loop bridge requests a frame while unpaused entries remain and on the tick that
  finishes one; paused-only, settled-fill-only, and idle ticks request nothing.

## 8. Interruption and cancellation

- Retargeting preserves continuity (position/velocity for springs).
- Removing a node cancels its animations and releases controller resources.
- Changing `animation`/`transition` declarations cancels/restarts per CSS semantics.
- Shutdown cancels all animations; no callbacks after the runtime stops.
- Animations never hold raw pointers to destroyed nodes; use stable ids/handles.

## 9. Determinism and tests

- Fixed timestep, fixed start time, fixed inputs.
- Sample at exact times (0, duration/2, duration) and compare values with tolerance.
- Golden images (deterministic reference images, see [glossary](../reference/glossary.md)) for visual transitions at fixed frames.
- Tests: easing curves, cubic-bezier, spring convergence, interruption/retarget, fill modes,
  reduced motion, hidden/occluded pause. The runtime test map is in
  [`animation-runtime.md`](animation-runtime.md#14-tests); per-executable coverage is listed in
  [`testing.md`](testing.md) and [`../guides/testing.md`](../guides/testing.md).
- No wall clock, random seeds, or system animation settings in goldens (inject them).

## 10. Diagnostics

Record:

- active animation count and names
- sampled value/clamped time per frame (debug)
- dropped animation frames
- spring energy/convergence
- reduced-motion/power-policy decisions

## 11. Deferred/open questions

- CSS `animation-composition` and additive animations.
- Scroll-driven animations.
- Shared-element transitions (window-root view transitions: see [`view-transitions-v1.md`](../specs/view-transitions-v1.md)).
- Whether springs are a CSS extension or a component-only API.
- Wide-gamut color interpolation (V1 uses sRGB only).
- Exact per-platform overscroll profile tuning.
- Haptics/audio feedback coupled to overscroll.
