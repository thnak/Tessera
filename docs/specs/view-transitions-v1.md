# View transitions v1 (global UI transitions)

Status: proposed (Big Step B2/C; custom effects V1.1). Decision: ADR-035. Companion to
[`animation-v1.md`](animation-v1.md), [`stylesheet-v1.md`](stylesheet-v1.md),
[`shader-manifest-v1.md`](shader-manifest-v1.md), [`custom-elements-v1.md`](custom-elements-v1.md),
[`overlays-and-layers-v1.md`](overlays-and-layers-v1.md), and
[`../design/dirty-rendering.md`](../design/dirty-rendering.md).

A **view transition** animates the whole window from the old UI to the new UI after an atomic
state change. Typical uses: theme switch with a circle reveal, route/locale/density switch with a
crossfade or wipe, a stylised glitch. The API is deliberately shaped like the web View Transitions
API (`document.startViewTransition`, `::view-transition-old/new(root)`) so web developers and
design-time previews map 1:1 onto the native engine.

## 1. Scope

- One transition at a time, targeting the window `root`.
- Built-in effects: `crossfade` (default), `circle-reveal`, `wipe`, `slide`.
- Developer effects: CSS keyframes on the `::view-transition-*` pseudo-elements (V1) and
  build-time shader effects declared in `ui/effects.json` (V1.1).
- Reduced motion, power policy, interruption, determinism, tests.
- Not in V1: per-element `view-transition-name` (shared elements), nested/scoped transitions,
  async update callbacks, multi-pass or feedback effects, runtime shader source.

## 2. Principles

- **State commits first.** The update runs synchronously and the new UI is the truth for input,
  focus, semantics, and automation. The old UI is a non-interactive image (same rule as
  `animation-v1.md` §8).
- **Native-first.** No JS runtime. The compiler lowers `startViewTransition` to C++ (§3).
- **Deterministic.** Progress comes from the simulation clock; `seed` is derived, never random.
- **Bounded and optional.** Textures are budgeted; failure to allocate degrades to an instant
  update, never to an error or a stuck frame.
- **Idle afterwards.** Zero presents/submissions after the transition finishes (ADR-019).

## 3. API

### 3.1 JSX/TS (design DSL)

```tsx
const vt = startViewTransition(() => setTheme(next), {
  effect: "circle-reveal",        // built-in id or manifest effect id
  origin: "pointer",              // "pointer" | "center" | { x, y } in px
  duration: "slow",               // token or ms
  easing: "ease-in-out",
  reducedMotion: "skip",          // "skip" (default) | "crossfade"
});
vt.finished.then(() => {});       // resolves at the tick boundary after the last frame
vt.skip();                        // jump to the end state
```

Tier-1 rules for the callback: a closure whose body contains only hook setters (`setX(...)`) and
pure expressions. Anything else (I/O, `await`, effects, refs) is rejected loudly (AGENTS.md
non-negotiable 8). `async` callbacks are rejected in V1.

### 3.2 Generated C++

```cpp
namespace tessera::ui::animation {
struct ViewTransitionSpec {
    EffectId effect = EffectId::Crossfade;     // built-in or manifest-generated id
    Origin origin = Origin::Center();          // pointer | center | point(px)
    Duration duration = Duration::Token(DurationToken::Slow);
    Easing easing = Easing::EaseInOut();
    ReducedMotion reduced = ReducedMotion::Skip;
    EffectParams params;                       // bounded, validated against the manifest
};

class ViewTransition {
public:
    static ViewTransition start(UpdateFn update, const ViewTransitionSpec& spec);
    Completion ready();      // old captured, update applied, first frame composed
    Completion finished();   // all frames presented, resources released
    void skip();             // idempotent
};
}
```

- `UpdateFn` is a synchronous `void()` called on the UI thread at a tick boundary.
- Completions are delivered through the bounded completion queue (ADR-021); no detached tasks.
- An exception in `update` is routed to the nearest error boundary (ADR-025) and the transition
  is skipped (state change that happened before the throw is kept, as for any event handler).
- Developer C++ may call `ViewTransition::start` from a `[DEVELOPER_LOGIC_START: bind::id]`
  region; it survives regeneration like any developer logic.

## 4. Lifecycle

```text
Idle -> Capturing -> Updating -> Composing -> Finished -> Idle
          |            |            |
          +------------+------------+--> Skipped (any error/cancel) -> Idle
```

1. **Capturing** (tick boundary): obtain the *old image* = the last presented frame if its
   target is still valid and clean, else render the current tree to texture `A`. In
   deterministic mode always re-render at this tick. Pending input for this tick is already
   drained.
2. **Updating**: run `update()`. Style transitions/keyframes are **not started** for the
   style recalc caused by this update (`suppressTransitions`), so the theme change is not
   animated twice. Layout/style/paint run normally for the new tree.
3. **Composing**: render the new tree into texture `B`; each tick compose `A` + `B` with the
   effect at `progress = clamp((now - start) / duration)` through the easing. Everything is
   sampled by the fixed-step clock before style/layout/paint.
4. **Finished**: free `A`/`B` through resource lifecycle retirement (frames in flight),
   restore normal direct rendering, drop to idle. Damage on the last tick is the full window;
   afterwards no wake source remains.

Input, focus, back/dismiss, and automation always see the new tree. The old image has
`pointer-events: none` and no semantics node. A transition never changes layout semantics or
automation IDs.

## 5. Pseudo-elements and CSS

The UA generates a fixed tree during Composing:

```text
::view-transition                      (window-sized, above all layers)
 └─ ::view-transition-group(root)
     └─ ::view-transition-image-pair(root)
         ├─ ::view-transition-old(root)   (texture A)
         └─ ::view-transition-new(root)   (texture B)
```

Default UA styles: `old` and `new` are opaque, full window; the built-in effect animation is
applied to them. Developers override with normal `animation`/`@keyframes`:

```css
::view-transition-old(root), ::view-transition-new(root) { animation: none; }
::view-transition-old(root) { z-index: 1; }
::view-transition-new(root) { z-index: 2;
  animation: reveal var(--vt-duration) ease-in-out both; }

@keyframes reveal {
  from { clip-path: circle(0px at var(--vt-x) var(--vt-y)); }
  to   { clip-path: circle(var(--vt-r) at var(--vt-x) var(--vt-y)); }
}
```

UA-injected custom properties on `:root` while Composing: `--vt-x`, `--vt-y` (origin px),
`--vt-r` (distance from origin to the farthest window corner), `--vt-w`, `--vt-h`,
`--vt-duration`, `--vt-progress` (0..1, read-only, for `calc()` use).

V1 CSS subset (anything else on these pseudo-elements is a loud diagnostic):

| Property | V1 | Notes |
|---|---|---|
| `animation*`, `@keyframes` | yes | Fixed-step sampling |
| `opacity`, `transform` | yes | Paint-only |
| `clip-path` | `circle()`, `inset([round])`, `ellipse()` | Only on view-transition pseudos; rendered with an SDF mask in the compositor; same shape function required for interpolation |
| `mix-blend-mode` | no | later |
| `filter`, `mask`, gradients | no | later |
| `view-transition-name` | `root` only | other names are rejected (shared elements deferred) |
| `view-transition-effect` | V1.1 | Tessera extension, selects a manifest effect |

Pseudo-elements are not selectable by app code, are not in the scene graph, and are absent from
semantics and automation snapshots.

## 6. Built-in effects

| Effect | Behavior | Implementation |
|---|---|---|
| `crossfade` | `old` fades out, `new` fades in | opacity |
| `circle-reveal` | `new` revealed by a growing circle from `origin` | `clip-path: circle` SDF mask |
| `wipe` | `new` revealed by a moving straight edge (`direction` param) | `inset()` mask |
| `slide` | `old` and `new` translate together | transform |

Built-ins are generated as the same keyframes a developer could write, so the custom-CSS path
and the built-in path share one implementation and one set of goldens. `circle-reveal` needs only
a rounded-rect SDF with radius = half the side, which the batched 2D renderer already has; no path
rasterizer is introduced.

## 7. Shader effects (V1.1)

For effects that cannot be expressed as opacity/transform/clip (glitch, RGB split, pixelate,
dissolve), developers declare a build-time effect. No runtime shader source (ADR-028).

`ui/effects.json`:

```json
{
  "manifestVersion": 1,
  "effects": [
    {
      "id": "glitch",
      "shader": "vt_glitch",
      "params": {
        "intensity": { "type": "float", "default": 0.6, "min": 0.0, "max": 1.0 },
        "slices":    { "type": "int",   "default": 12,  "min": 2,   "max": 64 }
      },
      "defaults": { "duration": 480 },
      "photosensitive": true
    }
  ]
}
```

Shader (`interface: "view-transition-effect"` in `shader-manifest-v1`): full-screen triangle,
no vertex buffers, one pass, fragment inputs:

| Binding | Content |
|---|---|
| texture `old`, `new` | premultiplied sRGB window images, sampler fixed by the framework |
| uniform block | `progress` (eased), `time` (sim seconds since start), `size` px, `origin` px, `scale` (DPI), `seed` (u32), `params` (declared floats/ints) |

- `seed = hash(transition_counter, effect_id)`; same inputs => same frames. No wall clock,
  no `rand`, no noise textures from disk beyond manifest resources.
- GLSL → SPIR-V (Vulkan) and WGSL (WebGPU) variants are validated against the manifest like any
  shader; both must pass the shared golden corpus.
- Limits: one pass, no storage buffers, no feedback/history textures, bounded uniform size,
  bounded `params` count; the framework owns render targets and sampler state.
- `photosensitive: true` marks flashing/strobing risk: such effects are disabled by
  reduced motion and low-power policy, and a lint rejects manifests whose declared peak
  luminance flashes exceed 3 per second.
- Developers can also parameterize an effect from CSS: `view-transition-effect: glitch(intensity 0.8)`
  on `:root` (Tessera extension; the JSX option wins when both are present).

## 8. Interruption, resize, lifecycle

- **Start during Composing:** capture the *currently composited output* as the new `A` (visual
  continuity, no jump), end the running transition immediately (its `finished` resolves), then
  proceed with Updating. At most one transition runs; a call arriving while a start is already
  pending in the same tick is coalesced (last `update` wins, earlier updates still run in order).
- **`skip()`:** jump to progress 1, release textures, resolve `finished`.
- **Resize/DPI/swapchain loss/fold posture change:** skip (state is already new; re-layout
  proceeds normally). Never keep stale-size textures.
- **Hidden/occluded/minimized:** skip rather than pause; resuming must not show a stale image.
- **Shutdown:** cancel, no callbacks after the runtime stops.
- **Route/overlay transitions** (`animation-v1.md` §8) remain local; a view transition is drawn
  above all overlay layers. If both occur, the local ones complete inside `B`.

## 9. Resources, power, accessibility

- Two window-sized textures (`A`, `B`) from the framework pool, budgeted by
  `viewTransition.maxBytes` (default 64 MiB; 4K RGBA8 pair ≈ 66 MB, so 4K needs a raised
  limit or degrades). Over budget or allocation failure => skip. Never allocate per frame.
- No `vkDeviceWaitIdle`/fence wait on the UI thread; retirement uses frames-in-flight rules.
- `prefers-reduced-motion`: `reducedMotion: "skip"` (default) applies the update instantly;
  `"crossfade"` allows a ≤120 ms opacity-only fade. Circle/wipe/slide/shader effects are never
  used under reduced motion.
- Power policy: shader effects fall back to `crossfade`; low-power/battery-saver => skip.
- The transition is a wake source while Composing; pacing follows `maxFps`/vsync like any
  animation. Damage is the full window while Composing; nothing afterwards.
- Accessibility: semantics tree reflects the new state immediately; screen readers receive no
  extra events for the animation. A theme switch must still announce the setting change through
  its control, not the transition.

## 10. Rendering notes

- Compose in the batched 2D renderer as two textured full-window quads plus an SDF mask for
  clip effects; shader effects replace the quad pass.
- No backdrop filter/blur is introduced (damage tracking assumptions in `dirty-rendering.md`
  stay valid; Composing simply forces full damage).
- Color: sRGB in V1 (matches `animation-v1.md` §4). Alpha is premultiplied.
- Text in `A`/`B` is rasterized at native scale; no resampling unless `scale` changes mid-flight
  (which skips, §8).
- WebGPU (Wasm viewer) uses the same lifecycle and pass structure. The design-time preview must
  use the engine's implementation, not the browser's `document.startViewTransition`, so preview
  and native match.

## 11. Diagnostics

Structured, bounded, optional (ADR-022): transition id, effect id, state transitions, duration,
frames composed, bytes allocated, skip reason (`reducedMotion`, `powerSave`, `resize`, `budget`,
`hidden`, `error`, `interrupted`), shader validation failures, CSS-subset violations,
photosensitive downgrade. No pixel contents in logs.

## 12. Determinism and tests

- Fixed dt/start time/origin/seed; no wall clock or system animation settings in goldens.
- Unit: lifecycle state machine, coalescing/interruption, skip reasons, suppressTransitions,
  progress/easing sampling, `--vt-r` computation, clip interpolation (same-shape rule),
  manifest validation (unknown id/param, range, traversal, oversize, photosensitive lint).
- Golden (lavapipe/software): each built-in effect and a sample shader effect at progress
  0, 0.25, 0.5, 0.75, 1.0; theme light→dark with circle origin at corners/center; RTL; DPI 1/2.
- Headless: new tree receives input and semantics during Composing; old image absent from
  snapshots; automation IDs unchanged; focus/back work mid-transition.
- Resource: no leak after skip/interrupt/resize; budget exceed => instant update; static scene
  after Finished produces zero presents/submissions.
- Cross-backend: Vulkan and WebGPU outputs match within the golden tolerance.
- Compiler: callback restricted to setters (rejects `await`, effects, refs); lowering golden.

## 13. Deferred/open questions

- Per-element `view-transition-name` and shared-element morphing.
- Scoped/nested transitions (a subtree instead of the window).
- Async `update` (promise) and "ready" gating on data.
- Multi-pass and feedback shader effects; `mix-blend-mode`, `filter`, general `clip-path`.
- Snapshot at reduced scale for shader effects to save memory.
- Optimization: draw `new` live into the frame for clip-only effects, avoiding texture `B`.
- Wide-gamut compositing.
