# ADR-035: View transitions (global UI transitions)

- Status: Accepted
- Date: 2026-11-08
- Related (number: subject): ADR-011: shader authoring; ADR-019: power-aware runtime; ADR-021:
  async runtime, UI thread never blocks; ADR-024: no JS/React runtime in production; ADR-028:
  custom native elements and build-time shaders; ADR-034: interaction animation and overscroll.
  Terms (view transition, old image, skip, tick boundary) are defined in
  [`view-transitions-v1.md`](../docs/specs/view-transitions-v1.md) section 0.1.

## Context

The animation design ([ADR-034](ADR-034-interaction-and-overscroll-animation.md)) covers node-level transitions, keyframes, springs, scroll/overscroll
and local route/overlay motion. There was no way to animate a window-wide change such as a theme
switch with a circle reveal or a glitch. A theme change only marked the affected nodes as needing
style recalculation (`StyleDirty`, see `stylesheet-v1.md` section 13.1) and swapped instantly; `clip-path`, filters and animated custom properties are outside the V1 stylesheet
subset; custom elements are leaf-scoped. On the web this is the View Transitions API
(`startViewTransition`, `::view-transition-old/new(root)`), and developers expect it.

## Decision

1. Add `docs/specs/view-transitions-v1.md` as the normative contract. `startViewTransition(update,
   options)` in the DSL lowers to `ViewTransition::start(UpdateFn, ViewTransitionSpec)` in C++.
   There is no JS runtime; callbacks are restricted to hook setters.
2. State commits synchronously first. The new tree owns input, focus, semantics and automation;
   the old UI is a non-interactive texture.
3. Lifecycle: capture old image -> run update (style transitions suppressed for that recalc) ->
   render new into an offscreen target -> compose with the effect on the fixed-step clock ->
   release and idle.
4. Developers define effects in two ways: CSS `animation`/`@keyframes` on a fixed
   `::view-transition-*` tree (V1) with a restricted `clip-path` subset (`circle`, `inset`,
   `ellipse`) rendered with the signed distance field (SDF) technique the batched 2D renderer
   already uses for rounded rectangles; and build-time shader effects declared in
   `ui/effects.json` (V1.1). "V1" is the first release scope ([glossary](../docs/reference/glossary.md), entry V1; which Big
   Steps it includes is an open decision); "V1.1" is the first
   release after V1, with no date set. Built-in effects:
   `crossfade`, `circle-reveal`, `wipe`, `slide`, implemented as the same keyframes.
5. In V1.1, shader effects use one pass, read only the old and new images, and have no runtime
   shader source (ADR-028). Photosensitive effects are flagged,
   linted, and disabled under reduced motion/low power.
6. Only the whole window (`root`) is supported in V1 and V1.1. `view-transition-name` other than
   `root`, async update functions, and subtree transitions are rejected with a diagnostic and are
   listed as open questions in the spec (section 13); no release is scheduled for them.
7. Bounded resources: two pooled window-sized textures under a new configuration key
   `viewTransition.maxBytes` (default 64 MiB). The transition is skipped (the new UI is shown at
   once) when the limit is exceeded, the window is resized, hidden or occluded, an error occurs,
   reduced motion is on, or battery-saver is active.
8. If a transition is started while another is running, the currently composed output becomes
   the new old image (no visual jump) and the running transition ends at once. Only one transition
   runs at a time. Several starts in the same tick run all update functions in call order and
   produce one transition that uses the options of the last call.
9. Deterministic: progress from the simulation clock, `seed` derived from a counter and effect id.
10. The design-time WebAssembly (Wasm) preview uses this engine's implementation and never the
    browser's own `document.startViewTransition`, so the preview matches native output.

## Consequences

- Web-compatible mental model for theme switches and similar window-wide effects without a
  JS/React runtime in production.
- Reuses SDF clipping, shader manifest, offscreen targets, resource lifecycle and goldens; adds a
  compositor pass and a small pseudo-element/CSS surface.
- Composing forces full-window damage and costs two window-sized textures; large displays may
  skip under the default budget.
- Photosensitivity and reduced-motion rules add policy and lint work.
- The CSS compatibility matrix gains a deliberately narrow `clip-path` exception.

## Alternatives

- Animate theme tokens per node via custom-property transitions: rejected for this goal (no
  circle/glitch, N concurrent animations, needs typed custom properties (`@property`), which `docs/specs/stylesheet-v1.md` section 9 says are
  not available until V1.1; per-token animation is not part of this decision).
- Developer-supplied runtime shaders/scripts: rejected (ADR-024, ADR-028, security).
- Browser `document.startViewTransition` in preview only: rejected (preview would diverge from
  native, breaks Big Step D acceptance).
- Per-element shared-element transitions in V1: not done, because node identity and layout interaction are not designed; it is an open
  question in `view-transitions-v1.md` section 13.
- Instant theme swap only: rejected (poor polish; expected by web developers).

## Follow-up

- Implement the `ViewTransition` state machine, texture pool use, compose pass and SDF clip masks.
- Add `::view-transition-*` parsing, UA-injected `--vt-*` properties and the `clip-path` subset.
- Add the `ui/effects.json` schema and validation, a `kind` field in `shader-manifest-v1.md`, and
  the `viewTransition.maxBytes` key in `runtime-config-v1.md`.
- Compiler: Tier-1 rule for `startViewTransition` callbacks and lowering.
- Goldens (all effects, RTL, DPI), headless, resource, budget and reduced-motion tests.
