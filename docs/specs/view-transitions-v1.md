# View transitions v1 (window-wide UI transitions)

Status: proposed. Proposed scope: built-in effects and CSS keyframe effects are in "V1" (the first
release scope, see the [glossary](../reference/glossary.md); its Big Step range is an open
decision); shader effects are "V1.1" (the first release after V1; no date is set).
Decision record: [ADR-035](../../decisions/ADR-035-view-transitions.md).

## 0. How to read this document

### 0.1 Terms

| Term | Meaning in this document |
|---|---|
| View transition | An animation of the whole window from the old UI image to the new UI image after one state change |
| Old image | A texture holding what the window showed before the state change (called texture `A`) |
| New image | A texture holding the UI after the state change, rendered offscreen (called texture `B`) |
| Update function | A synchronous function that performs the state change (for example `setTheme(next)`) |
| Tick | One fixed-step update of the runtime loop, see [`game-loop.md`](../design/game-loop.md) |
| Tick boundary | The point between two ticks; code that must not run mid-tick runs here |
| Simulation clock | The loop's fixed-step clock; never the wall clock, see [`game-loop.md`](../design/game-loop.md) |
| Skip | Apply the final state at once: render the new UI directly, release both textures, resolve `finished` |
| Damage | The screen region that must be redrawn, see [`dirty-rendering.md`](../design/dirty-rendering.md) |
| SDF | Signed distance field; the shader technique the batched 2D renderer already uses for rounded rectangles, see [`renderer.md`](../design/renderer.md) |
| Framework default styles | Styles built into Tessera that apply before any developer stylesheet (the equivalent of a browser user-agent stylesheet) |
| Reduced motion | The operating-system setting exposed as `prefers-reduced-motion`, handled as in [`animation-v1.md`](animation-v1.md) §4 |

### 0.2 References used by number

| Reference | Subject |
|---|---|
| [ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md) | Protected regions: developer C++ that code generation must preserve |
| [ADR-019](../../decisions/ADR-019-power-aware-runtime.md) | Power-aware defaults; a static UI must produce no presents |
| [ADR-021](../../decisions/ADR-021-async-runtime.md) | Async runtime; the UI thread never blocks; completions go through a bounded queue |
| [ADR-022](../../decisions/ADR-022-diagnostics-and-profiling.md) | Structured, bounded diagnostics |
| [ADR-024](../../decisions/ADR-024-native-first-boundaries.md) | No JavaScript/React runtime in production |
| [ADR-025](../../decisions/ADR-025-error-boundaries.md) | Exceptions are caught at error boundaries |
| [ADR-028](../../decisions/ADR-028-custom-native-elements.md) | Custom native elements; shaders are compiled at build time only |
| [ADR-035](../../decisions/ADR-035-view-transitions.md) | The decision this document implements |
| [`compiler.md`](../design/compiler.md) §4 | "Tier 1" rules: checks the compiler applies to React-style source |
| [`shader-manifest-v1.md`](shader-manifest-v1.md) | How shaders are declared and validated for Vulkan and WebGPU |
| [`resource-lifecycle-v1.md`](resource-lifecycle-v1.md) | Texture pool, retirement of GPU resources while frames are in flight |
| [WCAG 2.3.1](https://www.w3.org/TR/WCAG22/#three-flashes-or-below-threshold) | Three Flashes or Below Threshold: no more than three flashes in any one-second period |

## 1. Purpose and scope

A view transition makes a window-wide change (theme switch, locale switch, route switch) look
like a designed effect (circle reveal, crossfade, glitch) instead of an instant swap. The API
follows the web View Transitions API (`document.startViewTransition`, `::view-transition-old`,
`::view-transition-new`) so that web developers can reuse that knowledge. Tessera does not
promise identical behavior to browsers; this document is the contract.

In scope:
- One transition at a time, applied to the whole window (`root`).
- Built-in effects: `crossfade` (default), `circle-reveal`, `wipe`, `slide`.
- Developer effects written as CSS `@keyframes` on the `::view-transition-*` pseudo-elements (V1).
- Developer effects written as build-time shaders declared in `ui/effects.json` (V1.1).
- Reduced motion, low-power behavior, interruption, resource limits, determinism, tests.

Out of scope in V1 and V1.1 (rejected with a diagnostic; no release is scheduled):
- Per-element `view-transition-name` in this spec (it is specified in [`element-transitions-v1.md`](element-transitions-v1.md), which extends this one).
- Transitions that cover only a subtree of the window.
- Async update functions (a function that returns a promise).
- Effects that need more than one shader pass or that read previous output.
- Shader source supplied at runtime.

## 2. Rules that apply to every transition

1. **State changes first.** The update function runs synchronously. After it returns, the new UI
   is the only UI that receives input, focus, semantics (accessibility) and automation IDs. The
   old image is a picture only. Input and back/dismiss are never blocked while a transition runs.
2. **No script runtime.** There is no JavaScript or React runtime in a production app
   ([ADR-024](../../decisions/ADR-024-native-first-boundaries.md)). The compiler turns the DSL
   call into C++ (section 3).
3. **Deterministic.** Progress is computed from the simulation clock. Random values are never
   used; the shader `seed` is computed from a counter (section 7).
4. **Bounded.** The transition uses at most two window-sized textures. If they cannot be
   allocated, the transition is skipped; this is not an error.
5. **Idle afterwards.** When a transition finishes, no presents and no GPU submissions occur
   until something else changes ([ADR-019](../../decisions/ADR-019-power-aware-runtime.md)).

## 3. API

### 3.1 DSL (JSX/TSX)

```tsx
const vt = startViewTransition(() => setTheme(next), {
  effect: "circle-reveal",   // built-in id (section 6) or an id from ui/effects.json (section 7)
  origin: "pointer",         // "pointer" | "center" | { x, y }; see the origin rule below
  duration: "slow",          // token (fast=120 ms, normal=200 ms, slow=320 ms) or a number of ms
  easing: "ease-in-out",     // any easing listed in animation-v1.md section 4
  reducedMotion: "skip",     // "skip" (default) or "crossfade"
});
vt.finished.then(() => {});  // resolves at a tick boundary after the last frame is presented
vt.skip();                   // see "Skip" in section 0.1; safe to call more than once
```

Origin rule:
- `"pointer"`: the window position (device-independent pixels) of the most recent pointer-down or
  pointer-up event handled before the update function was called. If there is none (for example,
  the update came from a keyboard shortcut or a timer), the window center is used.
- `"center"`: the window center.
- `{ x, y }`: a position in window coordinates, device-independent pixels.

In the example, `setTheme` is the setter returned by the library hook `useThemeState()`
([`jsx-subset-v1.md`](jsx-subset-v1.md) section 7.1).

The update function must contain only calls to state setters of hooks (for example `setX(...)`)
and pure expressions. Any other construct (I/O, `await`, `useEffect`, refs) is a compile error
under the Tier 1 rules in [`compiler.md`](../design/compiler.md) §4. An `async` update function is
also a compile error.

### 3.2 Generated C++

```cpp
namespace tessera::ui::animation {
struct ViewTransitionSpec {
    EffectId effect = EffectId::Crossfade;     // built-in id, or an id generated from ui/effects.json
    Origin origin = Origin::Center();          // Pointer, Center, or a point in px
    Duration duration = Duration::Token(DurationToken::Slow);
    Easing easing = Easing::EaseInOut();
    ReducedMotion reduced = ReducedMotion::Skip;
    EffectParams params;                       // limited to the keys and ranges in the manifest
};

class ViewTransition {
public:
    static ViewTransition start(UpdateFn update, const ViewTransitionSpec& spec);
    Completion ready();      // old image captured, update applied, first composed frame ready
    Completion finished();   // last frame presented and textures released
    void skip();
};
}
```

- `UpdateFn` is a synchronous `void()` called on the UI thread at a tick boundary.
- `Completion` values are delivered through the bounded completion queue defined in
  [ADR-021](../../decisions/ADR-021-async-runtime.md). No detached tasks are used.
- If `update` throws, the exception goes to the nearest error boundary
  ([ADR-025](../../decisions/ADR-025-error-boundaries.md)) and the transition is skipped. State
  changes made before the throw are kept, as for any event handler.
- Developer C++ may call `ViewTransition::start` inside a protected region
  (`[DEVELOPER_LOGIC_START: bind::id]`, [ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md)).

## 4. Lifecycle

```text
Idle -> Capturing -> Updating -> Composing -> Finished -> Idle
          |            |            |
          +------------+------------+--> Skipped (error, cancel, or any rule in section 8/9) -> Idle
```

"Composing" means the old image and the new image are drawn together by the effect.

1. **Capturing** (at a tick boundary, after this tick's input is processed): the old image is the
   last presented frame if its render target is still valid and has no pending damage;
   otherwise the current tree is rendered into texture `A`. In deterministic mode the current
   tree is always re-rendered into `A`.
2. **Updating**: call the update function. For the style recalculation caused by this call only,
   CSS transitions and keyframe animations are not started. This prevents the change from being
   animated twice (once per node and once by the view transition). Style, layout and paint then
   run for the new tree as usual.
3. **Composing**: render the new tree into texture `B`. On each tick, draw `A` and `B` with the
   effect at `progress = easing(clamp((now - start) / duration, 0, 1))`, where `now` is the
   simulation clock. Progress is computed before style, layout and paint, as for other animations.
4. **Finished**: release `A` and `B` (section 9), return to direct rendering, and stop requesting
   frames. The last composed tick has full-window damage; after it, nothing remains scheduled.

During Composing, the old image has `pointer-events: none` and has no accessibility node. The
transition never changes layout or automation IDs.

## 5. Pseudo-elements and CSS

During Composing, Tessera creates this fixed tree above all overlay layers:

```text
::view-transition                       (window-sized)
 └─ ::view-transition-group(root)
     └─ ::view-transition-image-pair(root)
         ├─ ::view-transition-old(root)   (shows texture A)
         └─ ::view-transition-new(root)   (shows texture B)
```

The framework default styles make `old` and `new` opaque and window-sized, and apply the
built-in effect's keyframes (section 6). A developer overrides them with normal `animation`
and `@keyframes` rules that use the same selectors:

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

Custom properties that Tessera sets on `:root` while Composing:

| Property | Value |
|---|---|
| `--vt-x`, `--vt-y` | Origin position in px (section 3.1 origin rule) |
| `--vt-r` | Distance in px from the origin to the farthest window corner |
| `--vt-w`, `--vt-h` | Window size in px |
| `--vt-duration` | Duration in ms, with unit (for example `320ms`) |
| `--vt-progress` | Eased progress, number from 0 to 1; read-only; usable inside `calc()` |

Properties allowed on these pseudo-elements in V1. Any other property is a diagnostic:

| Property | Allowed | Note |
|---|---|---|
| `animation*`, `@keyframes` | yes | Sampled on the simulation clock |
| `opacity`, `transform` | yes | Paint only; no layout |
| `clip-path` | `circle()`, `inset()` (optionally with `round`), `ellipse()` only | Only on these pseudo-elements. Drawn with an SDF mask. Both keyframes must use the same function to be interpolated |
| `z-index` | yes | Orders `old` and `new` |
| `mix-blend-mode`, `filter`, `mask`, gradients | no | Not supported in V1 or V1.1 |
| `view-transition-name` | `root` only | Any other value is a diagnostic |
| `view-transition-effect` | V1.1 | Tessera extension: selects an effect from `ui/effects.json` (section 7) |

These pseudo-elements cannot be selected from app code, are not nodes in the scene graph, and do
not appear in semantics or automation snapshots.

## 6. Built-in effects

| Effect | Visible result | How it is defined |
|---|---|---|
| `crossfade` | Old image fades out while the new image fades in | `opacity` keyframes |
| `circle-reveal` | A growing circle centered on the origin shows the new image | `clip-path: circle()` keyframes on `new` |
| `wipe` | A straight edge moves across the window and shows the new image | `clip-path: inset()` keyframes on `new`; `direction` param: `left`, `right`, `up`, `down` |
| `slide` | Old and new images move together | `transform` keyframes |

Each built-in effect is a `@keyframes` rule in the framework default styles. A developer rule
with the same pseudo-element selector overrides it. `circle-reveal` uses the SDF already used for
rounded rectangles, with radius equal to the circle radius; no general path rasterizer is added.

## 7. Shader effects (V1.1)

Use a shader effect when opacity, transform and clip cannot express the look (glitch, RGB split,
pixelate, dissolve). Shaders are compiled at build time; runtime shader source is rejected
([ADR-028](../../decisions/ADR-028-custom-native-elements.md)).

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

`duration` is in milliseconds. `shader` must be the `name` of an entry in the shader manifest
([`shader-manifest-v1.md`](shader-manifest-v1.md)) whose `kind` is `view-transition-effect`
(section 3.1 of that file); any other kind is a build error. An effect declares at most 8 `params`; more
is a build error.

Shader contract: one full-screen triangle, no vertex buffers, one pass.

| Binding | Content |
|---|---|
| texture `old`, texture `new` | Premultiplied-alpha sRGB images of the window; sampler chosen by the framework |
| uniform block | `progress` (eased, 0 to 1), `time` (seconds on the simulation clock since start), `size` (px), `origin` (px), `scale` (DPI scale), `seed` (unsigned 32-bit integer), `params` (the declared numbers, in declaration order; an `int` param is converted to `float`; unused slots are 0). The byte layout is in [`shader-manifest-v1.md`](shader-manifest-v1.md) section 3.1 |

- `seed = hash(transition_counter, effect_id)`. `transition_counter` is the number of view
  transitions started since the runtime started. Identical inputs produce identical frames. No
  wall clock, random function, or noise texture outside the manifest is used.
- Shader sources exist in GLSL (compiled to SPIR-V for Vulkan) and WGSL (WebGPU), validated against
  the manifest. Both must pass the shared golden corpus
  ([`golden-tests-v1.md`](golden-tests-v1.md)).
- Not allowed: more than one pass, storage buffers, textures that keep previous output, shader
  access to render targets or samplers other than those listed above.
- `photosensitive: true` means the effect can flash. Such an effect is turned into `crossfade`
  when reduced motion is on or low-power mode is active (section 9). A golden test measures its
  frame sequence against the WCAG 2.3.1 flash threshold (no more than three flashes in any
  one-second period); an effect that fails the test cannot be added to the corpus.
- CSS selection (Tessera extension): `view-transition-effect: glitch(intensity 0.8)` on `:root`.
  If both the DSL option and the CSS property are present, the DSL option is used.

## 8. Interruption and lifecycle events

| Event while Composing | Result |
|---|---|
| `start` is called again | The currently composed output becomes the new old image (no visual jump). The running transition finishes at once (its `finished` resolves). Then the new transition starts at Updating. |
| `start` is called more than once in the same tick | All update functions run in call order. One transition runs, using the options of the last call. |
| `skip()` is called | Skip (section 0.1) |
| Window resize, DPI change, swapchain loss, fold-posture change | Skip. The new UI is already committed and re-lays out normally. Stale-size textures are never kept. |
| Window hidden, occluded, or minimized | Skip, not pause, so a resumed window never shows a stale image |
| Runtime shutdown | Cancel; no callback runs after the runtime stops |
| Route or overlay animation inside the new UI | Runs normally inside the new image. A view transition is drawn above all overlay layers. |

## 9. Resources, power and accessibility

Resources:
- Two window-sized textures (`A`, `B`) come from the framework texture pool
  ([`resource-lifecycle-v1.md`](resource-lifecycle-v1.md)) and are released by that document's
  retirement rules (a texture is freed only after the GPU frames that use it complete).
- Memory limit: configuration key `viewTransitionMaxMb` ([`runtime-config-v1.md`](runtime-config-v1.md)),
  default 64 MiB, for `A` plus `B`. A 3840x2160 RGBA8 texture is 33,177,600 bytes, so `A` plus `B` is
  66,355,200 bytes (63.3 MiB) and fits the default; a 5120x2880 window needs 117,964,800 bytes
  (112.5 MiB), so its transition is skipped unless the key is raised.
- If the limit is exceeded or allocation fails, the transition is skipped.
- No textures are created per frame. The UI thread never waits on a GPU fence or on device idle
  ([ADR-021](../../decisions/ADR-021-async-runtime.md)).

Reduced motion (OS setting on):
- `reducedMotion: "skip"` (default): apply the update with no animation.
- `reducedMotion: "crossfade"`: an opacity-only crossfade of at most 120 ms.
- `circle-reveal`, `wipe`, `slide` and shader effects are never run.

Low power (power policy reports low-power or battery-saver, see
[`performance.md`](../design/performance.md)):
- Shader effects run as `crossfade`.
- If battery-saver is active, the transition is skipped.

Scheduling and accessibility:
- While Composing, the transition requests frames like any animation and follows `maxFps` and
  vsync. Damage is the full window while Composing and none afterwards.
- The accessibility tree describes the new UI immediately. Screen readers receive no event for
  the animation itself. A theme switch is announced by the control that changed it, not by the
  transition.

## 10. Rendering notes

- Compositing uses the batched 2D renderer: two textured full-window quads and, for clip
  effects, an SDF mask. A shader effect replaces the quads' fragment shader.
- No blur or backdrop filter is added; the damage rules in `dirty-rendering.md` are unchanged
  (Composing simply marks the whole window as damaged).
- Color: sRGB; alpha is premultiplied.
- Text inside `A` and `B` is rasterized at the window's current DPI scale. DPI changes skip
  the transition (section 8), so textures are never resampled.
- The WebGPU backend (browser designer) uses the same lifecycle and passes. The browser designer
  must use this engine implementation and must not call the browser's own
  `document.startViewTransition`, so the preview matches native output.

## 11. Diagnostics

Records follow [ADR-022](../../decisions/ADR-022-diagnostics-and-profiling.md) (structured,
bounded, optional): transition id, effect id, lifecycle state changes, duration, composed frame
count, texture bytes, skip reason, shader validation failures, CSS-subset violations,
photosensitive downgrade. Skip reasons: `reducedMotion`, `lowPower`, `resize`, `budget`, `hidden`,
`error`, `interrupted`, `userSkip`. Pixel contents are never logged.

## 12. Tests

All tests use a fixed time step, fixed start time, fixed origin and fixed seed. No wall clock and
no system animation settings are used in golden tests.

- Unit: lifecycle states; interruption; same-tick calls; each skip reason; style transitions not
  started during Updating; progress and easing values at fixed times; `--vt-r` value;
  clip-path interpolation (same function required); manifest validation (unknown id, unknown
  param, out-of-range value, path traversal, oversize file).
- Golden (software rendering with Mesa lavapipe, see [`golden-tests-v1.md`](golden-tests-v1.md)):
  each built-in effect and one sample shader effect at progress 0, 0.25, 0.5, 0.75 and 1.0; light
  to dark theme with the origin at each corner and at the center; right-to-left layout; DPI
  scale 1 and 2.
- Headless ([`headless-automation-v1.md`](headless-automation-v1.md)): during Composing, the new
  tree receives input and appears in semantics snapshots; the old image does not; automation IDs
  are unchanged; focus and back work.
- Resources: no leaked textures after skip, interruption or resize; budget exceeded gives an
  instant update; after Finished a static scene produces zero presents and zero submissions.
- Backends: Vulkan and WebGPU outputs match within the tolerance defined in
  [`golden-tests-v1.md`](golden-tests-v1.md).
- Compiler: update functions containing anything except setters are rejected; the lowering to
  C++ matches a stored golden file.

## 13. Open questions (no decision yet)

- Shared-element transitions with `view-transition-name`: specified in [`element-transitions-v1.md`](element-transitions-v1.md) (proposed).
- Transitions limited to a subtree.
- Async update functions and waiting for data before starting.
- Multi-pass or feedback shader effects; `mix-blend-mode`, `filter`, general `clip-path`.
- Rendering `A` at reduced resolution for shader effects to save memory.
- Drawing the new UI directly to the frame (without texture `B`) for clip-only effects.
- Wide-gamut color compositing.
