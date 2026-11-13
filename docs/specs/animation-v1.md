# Animation and interaction effects v1

Status: implementation started for Big Steps B2/C (defined in
[AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step)). `ui/animation/keyframes.hpp`
samples parsed `@keyframes` at fixed-step time with linear numeric/unit interpolation,
iteration/direction, delay, and fill modes. `AnimationRuntime` tracks active entries, advances them
with `tick(dt)`, reports samples/finished ids/dirty domains, and runs completion callbacks on the
tick boundary. Easing supports linear, `ease*` presets, `cubic-bezier()`, and `steps()` with all
jump positions; matching color keyframes interpolate in sRGB. `Spring` integrates
stiffness/damping/mass with semi-implicit Euler, preserves
position/velocity on retarget, and snaps to target below epsilon thresholds. `SpringRuntime` turns
active spring entries into property declarations, retargets them, and reports dirty/wake state.
Animation samples overlay `ComputedStyle` per target; later samples win, multiple properties merge,
and properties without an animated declaration are preserved. `transition` shorthand parsing splits
comma lists outside functional parentheses and accepts property/duration/delay/easing in any order,
`ms`/`s` units, `cubic-bezier()`, `steps()`, `none`, and empty lists. Exact-property transitions win
over `all`; starting one builds 0→1 keyframes and runs through `AnimationRuntime` with the parsed
delay/easing and forwards fill. Diffing two `ComputedStyle` snapshots starts transitions only for
properties present in both and changed, preserving original units and color values. Longhand
expansion accepts `transition-property`, `transition-duration`, `transition-delay`, and
`transition-timing-function` comma lists, cycling shorter lists and defaulting omitted values.
Starting a
transition supersedes the active transition for the same target and property, so the new segment
continues from the current sampled value. Ticking applies samples over the target's
`ComputedStyle`, preserving untouched properties and reporting layout/paint/render dirty domains.
The `animation` shorthand parser accepts name, duration, timing function, delay, iteration count,
direction, fill mode, and play state in any order across comma-separated lists. The eight longhand
properties accept independent comma lists, cycle shorter lists, and default omitted values.
Starting from computed style reads the animation longhands when `animation-name` is present and
otherwise falls back to the `animation` shorthand. Starting an
animation maps the parsed spec onto `AnimationRuntime` timing, skips `none`/empty/zero-iteration
entries, starts `paused` entries held at their initial sample, and keeps the final sample when
fill-forwards applies. Completed forward/both fills re-emit that sample without waking the loop
until the entry is stopped or superseded. Keyframes resolve by exact
name (last duplicate wins), batch starts skip specs whose block is missing, and ticking samples
overlay onto `ComputedStyle` with layout/paint/render dirty flags while the runtime stays awake.
Pausing holds an entry's current sample without advancing time or waking the loop; resuming
continues from that point. Play-state changes can target one animation by exact target/name
pair without disturbing its siblings. Applying computed-style play state updates matching active
animations without starting, stopping, or restarting them. Ensuring style animations reuses
matching active entries, skips names with retained fills, and starts only missing names. Syncing
style animations also stops active or retained-fill names that are absent from the computed style.
The runtime-loop bridge requests a frame while animations are running and on the tick that finishes
one, without waking for paused-only or retained-fill samples. Stopping a target removes all of its
entries, paused ones included;
stopping a target/name pair removes only that animation and its retained fill. Restarting a
target/name pair or a single id rewinds matching active entries and revives retained fills from
their start.
Completion callbacks run after forwards/both fills are retained, so clearing, stopping, or replacing
from a callback removes or supersedes that fill. Samples and dirty domains are produced after
callbacks, so the tick result reflects the post-callback runtime without stale entries. Target/name
state exposes active and settled counts for style reconciliation. "V1" means the first-release scope ([AGENTS.md](../../AGENTS.md), Non-negotiables rule 8).
Decision: [ADR-034](../../decisions/ADR-034-interaction-and-overscroll-animation.md) (interaction
animation and overscroll stretch/spring-back). Companion to
[`animation.md`](../design/animation.md), [`animation-runtime.md`](../design/animation-runtime.md)
(runtime assembly, tick ordering, pause/fill/stop/restart, loop bridge, reduced motion),
[`game-loop.md`](../design/game-loop.md), [`scroll-and-gestures-v1.md`](scroll-and-gestures-v1.md),
and [`overlays-and-layers-v1.md`](overlays-and-layers-v1.md).

Animation is declarative motion sampled by the fixed-step runtime (a loop that advances the simulation in constant time slices, see [`game-loop.md`](../design/game-loop.md)). Interaction effects
(press/hover/focus, scroll fling/snap, overscroll stretch/spring-back, route/overlay transitions)
use the same clock, interruption, reduced-motion, and determinism rules.

## 1. Scope

- Style transitions/keyframes/animated values/springs.
- Scroll fling, snap, scroll-into-view, scrollbar fade.
- Overscroll visual effects: stretch/bounce/glow and spring-back.
- Overlay/route enter/exit and focus/press feedback.
- Reduced motion, power policy ([ADR-019](../../decisions/ADR-019-power-aware-runtime.md), power-aware runtime defaults), interruption, tests.
- Window-wide transitions (theme switch, circle reveal, shader effects) are specified in
  [`view-transitions-v1.md`](view-transitions-v1.md) ([ADR-035](../../decisions/ADR-035-view-transitions.md), view transitions).
- Scroll-driven/shared-element animations are deferred; no release is assigned (see section 10).

## 2. Ownership

| Component | Owns | Does not own |
|---|---|---|
| Style animation | transitions, keyframes, interpolation | Layout/scroll physics |
| `AnimationRuntime` | active animations, fixed-step sampling, completion | Component state |
| `ScrollController` | fling/snap/overscroll state/physics | Shaping/paint details |
| `OverscrollEffect` renderer | stretch/translation/glow paint | Scroll offset/content layout |
| Navigation/overlay | transition timing/content | Global clock |
| HAL (hardware/OS abstraction layer, [`hal.md`](../design/hal.md)) | platform motion preference/profile | App animation policy |

## 3. Timeline and sampling

A tick is one fixed-step update of duration `dt` ([`game-loop.md`](../design/game-loop.md)). "Dirty" means flagged for recomputation in the style/layout/paint/render domains ([`dirty-rendering.md`](../design/dirty-rendering.md)).

```text
tick(dt):
  advance unpaused animations at fixed dt
  finish entries, retain forwards/both final samples
  run completion callbacks
  sample active entries, then retained fills (post-callback)
  write sampled values into visual state
  mark paint/render dirty (layout only if a layout property animates)
  keep awake only while unpaused entries remain
```

- Use the simulation clock, never wall time; deterministic mode injects start time and dt.
- Sampling happens before style/layout/paint; render may interpolate between ticks.
- An animation that only changes transform/opacity does not mark layout dirty.
- Completion callbacks run on the UI thread at a tick boundary; no detached tasks (AGENTS.md Non-negotiables rule 12, [ADR-021](../../decisions/ADR-021-async-runtime.md) (async runtime)).
- Hidden/occluded/minimized pauses animations by default; setting the runtime config key `pauseWhenOccluded=false` ([`runtime-config-v1.md`](runtime-config-v1.md)) allows them to continue.

## 4. Curves, springs, and tokens

- Easing: linear, ease*, cubic-bezier, steps; color in sRGB for v1.
- Springs: damped harmonic oscillator integrated at fixed dt (semi-implicit Euler).
- Standard duration tokens: `fast` 120 ms, `normal` 200 ms, `slow` 320 ms (defaults of the bundled component library, [`component-library.md`](../design/component-library.md);
  apps may override).
- A spring is defined by `(stiffness, damping, mass, initial_velocity)`; retargeting keeps
  position/velocity.
- `prefers-reduced-motion` (OS) -> no non-essential motion; functional changes are instant or
  a short opacity change. No parallax/stretch/bounce. Callers drive the runtime setter from
  `MediaContext::prefers_reduced_motion`: opacity-only animations clamp to 120 ms with no delay
  and one normal-direction iteration, while every other animation becomes instant. Springs snap
  to their target instead of integrating.

## 5. Interaction feedback

| Effect | Default | Notes |
|---|---|---|
| Press | scale/opacity 0.98 over `fast` | interruptible; no layout |
| Hover | color/elevation over `fast` | disabled on touch |
| Focus ring | opacity/scale 120 ms | visible immediately, no delay |
| Validation error | shake 160 ms max | respect reduced motion; one shake |
| List insert/remove | fade/size `normal` | keyed; no jump for existing items |
| Route/overlay | slide/fade `normal` | state changes immediately; animation is visual |
| Scrollbar | fade out after idle | no frame when hidden |

- Feedback must not be the only signal (accessibility/state still changes).
- Animations never block input/state commit; back/dismiss works during a transition.
- Shake/parallax are optional and disabled by reduced motion/power-save.

## 6. Scroll animation

- Fling/coast uses a bounded decay; velocity is clamped; stops at zero.
- Snap/paging chooses a deterministic target from position/velocity; reduced motion = immediate.
- `scroll_to`/`scroll_into_view` use a tween/spring with cancellation on user input.
- A new pointer down interrupts fling/snap and preserves continuity.
- Scroll-driven animations are not in V1 (section 10); do not couple layout to scroll progress.

## 7. Overscroll effects

Scroll offset remains clamped. Overscroll is a **separate visual state**:

```cpp
enum class OverscrollEffect { None, Glow, Stretch, Bounce, Platform };

struct OverscrollState {
    Vec2 visual;        // stretched/translated px, 0 at rest
    Vec2 velocity;      // px/s, visual state
    bool active;
    OverscrollEffect effect;
};

struct OverscrollProfile {
    OverscrollEffect effect = OverscrollEffect::Platform;
    float max_stretch_ratio = 0.12f;   // of viewport, per axis
    float max_stretch_px = 64.0f;      // hard cap
    float stiffness = 220.0f;
    float damping = 26.0f;
    float glow_radius_px = 24.0f;
};
```

Modes:

- `None`: clamp; no visual overscroll.
- `Glow`: edge glow; offset stays clamped.
- `Stretch`: content layer stretches/translates past the edge; sticky headers, scrollbars, and
  overlays are not stretched.
- `Bounce`: content layer translates past the edge (no scale).
- `Platform`: HAL profile. Proposed defaults: iOS/macOS = `Stretch`, Android = `Stretch`
  (glow on Android 11 (API level 30) and earlier; stretch on Android 12 (API level 31) and later), Windows/Linux = `None` for wheel/mouse, `Stretch` for touch if
  the platform's HAL profile supports it. Apps may override per `Scroll`.

The mode is selected by the `overscroll-effect` CSS property
([`stylesheet-v1.md`](stylesheet-v1.md) section 11): `none | glow | stretch | bounce | platform`,
not inherited, with the initial request `platform` (the values are exactly the `OverscrollEffect`
enumerators above, and there is no `auto` keyword). The scene stores the request and the scroll
controller derives whether the edge may animate (`contain`/`none` `overscroll-behavior` and
`overscroll-effect: none` suppress it); painting the stretch/bounce/glow state is the remaining
implementation slice of this section, and the controller today clamps the offset either way.

Drag beyond an edge:

```text
raw = max(0, pointer_beyond_edge)
max = min(viewport * max_stretch_ratio, max_stretch_px)
visual = max * raw / (raw + max)     // monotonic, asymptotically max
```

- The content is transformed around the opposite edge and clipped to the viewport.
- Sticky headers/scrollbars stay fixed; edge glow is painted by the container.
- No overscroll if content fits the viewport; no overscroll on the wrong axis.
- Nested scroll: only the last eligible container applies the effect; `overscroll-behavior` (the CSS property)
  controls chaining separately. `contain`/`none` do not animate past the edge.
- Mouse wheel/trackpad and touch use the same state but may have different HAL profiles.

Release/spring-back:

```text
a = -stiffness * visual - damping * velocity
velocity += a * dt
visual += velocity * dt
stop when |visual| < 0.5 px and |velocity| < 5 px/s
```

- Initial velocity comes from the release gesture, clamped; no fling while beyond the edge.
- A new pointer down catches the current visual value and cancels the spring without a jump
  (`raw = max * visual / (max - visual)` when `visual < max`).
- Content-size/axis/config changes cancel the spring and re-clamp.
- The effect never changes `ScrollState.offset` (defined in [`scroll-and-gestures-v1.md`](scroll-and-gestures-v1.md)), content size, focus, or automation IDs ([`automation-ids-v1.md`](automation-ids-v1.md)).

Rendering:

- Stretch/bounce applies to the content layer only; damage (the screen region that must be repainted, [`dirty-rendering.md`](../design/dirty-rendering.md)) is the union of old/new visual bounds.
- No relayout; transform/paint only. Text/glyph rendering may stretch with the layer.
- Glow is a paint primitive clipped to the container; no layout/GPU target allocation per frame.
- The effect stops requesting frames when at rest; hidden/occluded pauses it.

Reduced motion/accessibility:

- `prefers-reduced-motion` -> `None` (clamp immediately); no stretch/bounce/glow animation.
- The effect is never the only feedback; scroll position/state remains correct.
- Power-save (the low-power policy of [ADR-019](../../decisions/ADR-019-power-aware-runtime.md)) may disable glow/stretch before dropping frames.

## 8. Overlay/route transitions

- Enter/exit uses `normal` duration by default; reduced motion = instant/fade only.
- State commits before/independent of animation; input/back is not blocked.
- Interrupted transitions retarget from the current visual value, not the old endpoint.
- Focus is moved/restored at the logical commit boundary, not after the animation.
- A transition never changes layout semantics or automation state.

## 9. Determinism and tests

- Fixed dt/start time/inputs; no wall clock/random seeds.
- Unit: easing/spring convergence, retarget/interrupt, epsilon stop, reduced motion.
- Scroll: fling/snap/scroll-to cancel, overscroll resistance monotonic/asymptotic, release
  convergence, interruption without jump, nested/axis/RTL, wheel vs touch.
- Golden (reference-image comparison, [`golden-tests-v1.md`](golden-tests-v1.md)): stretch/bounce/glow at fixed frames, route/overlay transition frames, press/focus.
- Headless (no window, driven by [`headless-automation-v1.md`](headless-automation-v1.md)): assert visual overscroll state, zero at rest, no offset/layout change.
- No animation frame when at rest; no leak after node removal.

## 10. Deferred/open questions

- Scroll-driven animations. Shared-element transitions and enter/exit (presence) are in
  [`element-transitions-v1.md`](element-transitions-v1.md); window-root transitions are in
  `view-transitions-v1.md`.
- Exact per-platform overscroll profiles/curve tuning.
- Pull-to-refresh and custom overscroll effects (library).
- Additive composition. Animation of layout properties is specified for bounded cases in
  [`element-transitions-v1.md`](element-transitions-v1.md) section 3.7.
- Wide-gamut color interpolation.
- Haptics/audio feedback coupled to overscroll (platform).
