# Animation and transitions

Status: design for [Big Step B2](../../AGENTS.md#big-steps-group-work-dont-micro-step) (runtime slice). `src/ui/animation/` is a placeholder today.
Normative animation/interaction/overscroll contract:
[`animation-v1.md`](../specs/animation-v1.md).

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
  for each active animation:
    t = clamp((now - start) / duration, 0, 1)
    value = sample(t)
    write value into computed style
  mark layout/paint/render as needed
  if no active animations: idle (no wake)
```

- `tick` is one update of the fixed-step runtime loop ([`game-loop.md`](game-loop.md)); `now` is the fixed-step simulation clock, not wall time.
- `dt` is the fixed timestep (e.g. 16.666 ms); animation is frame-rate independent.
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
- Completion callbacks are delivered on the UI thread via the completion queue ([`async.md`](async.md)); no detached
  tasks.

## 7. Scheduling and power

- A frame with active animations is a wake source.
- No active animations -> no animation ticks; if nothing else is dirty, the loop idles.
- Hidden/occluded/minimized -> pause animations (unless the app configures otherwise; the setting is not named in this file).
- `powerPolicy` (runtime setting, see [`runtime-config-v1.md`](../specs/runtime-config-v1.md)) may reduce animation quality (e.g. fewer samples, no spring overshoot) before
  dropping frames.
- `prefers-reduced-motion` -> skip non-essential motion, keep functional state changes.
- Never busy-wait for the next animation sample; use the loop's deadline/wakeup.

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
  reduced motion, hidden/occluded pause.
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
