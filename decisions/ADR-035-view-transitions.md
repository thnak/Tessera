# ADR-035: View transitions (global UI transitions)

- Status: Accepted
- Date: 2026-11-08
- Related: ADR-011, ADR-019, ADR-021, ADR-024, ADR-028, ADR-034

## Context

The animation design (ADR-034) covers node-level transitions, keyframes, springs, scroll/overscroll
and local route/overlay motion. There was no way to animate a window-wide change such as a theme
switch with a circle reveal or a glitch. A theme change only marked `StyleDirty` and swapped
instantly; `clip-path`, filters and animated custom properties are outside the V1 stylesheet
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
   `ellipse`) rendered via the existing SDF; and build-time shader effects declared in
   `ui/effects.json` against a `view-transition-effect` shader interface (V1.1). Built-in effects:
   `crossfade`, `circle-reveal`, `wipe`, `slide`, implemented as the same keyframes.
5. No runtime shader source, one pass, no feedback in V1.1. Photosensitive effects are flagged,
   linted, and disabled under reduced motion/low power.
6. Only the window `root` is supported in V1. `view-transition-name` other than `root`,
   async update callbacks, and scoped transitions are deferred and rejected loudly.
7. Bounded resources: two pooled window-sized textures under `viewTransition.maxBytes`; over
   budget, resize, hidden, error, reduced motion or low power => skip to the new state.
8. Interruption captures the currently composited output as the new "old" image for continuity;
   one transition at a time, same-tick starts are coalesced.
9. Deterministic: progress from the simulation clock, `seed` derived from a counter and effect id.
10. The design-time Wasm preview uses the engine implementation, not the browser's native API.

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
  circle/glitch, N concurrent animations, needs `@property`; still kept as a later enhancement).
- Developer-supplied runtime shaders/scripts: rejected (ADR-024, ADR-028, security).
- Browser `document.startViewTransition` in preview only: rejected (preview would diverge from
  native, breaks Big Step D acceptance).
- Per-element shared-element transitions in V1: deferred (layout/identity complexity).
- Instant theme swap only: rejected (poor polish; expected by web developers).

## Follow-up

- Implement `ViewTransition` state machine, snapshot pool, compose pass and SDF clip masks.
- Add `::view-transition-*` parsing, UA-injected `--vt-*` properties and the `clip-path` subset.
- Add `ui/effects.json` schema/validation and the `view-transition-effect` shader interface.
- Compiler: Tier-1 rule for `startViewTransition` callbacks and lowering.
- Goldens (all effects, RTL, DPI), headless, resource, budget and reduced-motion tests.
