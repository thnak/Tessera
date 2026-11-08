# ADR-034: Interaction animation and overscroll effects

- Status: Accepted
- Date: 2026-11-07
- Related: [ADR-029](ADR-029-interaction-completeness.md) (interaction completeness), [ADR-033](ADR-033-app-completeness-gap-resolutions.md) (app-completeness gap resolutions)

## Context

The animation design covered CSS transitions, keyframes, animated values, and springs, but it did
not define interaction effects. Scroll overscroll was only "opt-in" with no visual/behavioral
contract. Real apps expect native-feeling feedback: drag past an edge, stretch/bounce with
resistance, release, and spring back. This must not change scroll offset, layout, focus, or
automation identity, and must remain deterministic/testable.

## Decision

1. `animation-v1.md` is the normative animation/interaction contract; the design doc is
   rationale/summary.
2. Overscroll is a separate visual state (`visual`, `velocity`) applied to the content layer.
   `ScrollState.offset` remains clamped.
3. Modes: `None`, `Glow`, `Stretch`, `Bounce`, `Platform`. Sticky headers, scrollbars, and
   overlays are outside the effect.
4. Drag beyond an edge uses asymptotic resistance:
   `visual = max * raw / (raw + max)`, capped by viewport ratio and px cap.
5. Release runs a damped spring to zero at fixed dt; a new pointer down catches the current
   visual value and cancels without a jump.
6. Platform profiles resolve via HAL (iOS/macOS stretch; Android stretch/legacy glow;
   Windows/Linux none for wheel, stretch for touch where supported). Apps may override.
7. `prefers-reduced-motion` disables stretch/bounce/glow and clamps immediately. The effect is
   never the only feedback.
8. Press/hover/focus/validation/list/route/overlay effects use the same fixed-step sampler and
   interruption rules. State commits before/independent of animation.
9. No relayout and no frames at rest; damage (the screen regions to repaint, see decision 4 of [ADR-018](ADR-018-dirty-rendering-and-frame-pacing.md) (dirty rendering and frame pacing)) is the visual union. The effect never changes
   content size, focus, selection, or automation IDs.

## Consequences

- Native-feeling overscroll without corrupting scroll semantics or automation.
- Platform profiles must be calibrated on real devices; tests use fixed profiles.
- Content-layer stretch can blur text temporarily; it is visual-only and returns to rest.
- More animation state to snapshot/diagnose and test.

## Alternatives

- Bounce the actual scroll offset: rejected (breaks clamped semantics, automation, focus).
- Relayout/stretch content geometry: rejected (cost/jank, text reflow).
- Platform-only effect with no framework contract: rejected (inconsistent/difficult to test).
- No overscroll effects: rejected (poor touch/native feel).

## Follow-up

- Implement `OverscrollState`/spring integrator and content-layer transform.
- Add platform profiles and per-`Scroll` overrides.
- Add resistance/release/interrupt/reduced-motion/golden tests.
- Calibrate profiles on iOS/Android/Windows/Linux hardware.
