# Responsive UI, safe areas, and fold postures

Status: design for Big Step B2/B3 (runtime slice and window/swapchain phases of [Big Step B](../../AGENTS.md#big-steps-group-work-dont-micro-step)). Depends on the stylesheet ([`stylesheet-v1.md`](../specs/stylesheet-v1.md)), layout ([`layout.md`](layout.md)), window-resize ([`window-resize.md`](window-resize.md)), and
OS-events ([`os-events-v1.md`](../specs/os-events-v1.md)) specs.

Responsive behavior is a function of **metrics**, not a separate UI mode. The same component
tree adapts to size, DPI, orientation, safe areas, multi-window, and fold posture. Mobile
surface/input/IME/declaration details are in [`mobile-integration.md`](mobile-integration.md).

## 1. Inputs

From `WindowMetrics` ([glossary](../reference/glossary.md); defined in [`os-events-v1.md`](../specs/os-events-v1.md)):

- logical/physical size, DPI/scale
- orientation
- safe area (notch/cutout/system bars/IME = input method editor, on-screen keyboard)
- work area (taskbar/dock/panels)
- fold: state, orientation, occlusion, separating, hinge bounds
- multi-window / PiP (picture-in-picture) / desktop windowing
- display id, refresh/VRR (variable refresh rate), HDR (high dynamic range)/color
- text scale, locale/layout direction, reduced motion, high contrast

## 2. Breakpoints and media queries

CSS `@media` V1 (first-release scope, see [AGENTS.md rule 8](../../AGENTS.md#non-negotiables); the normative list is in [`stylesheet-v1.md`](../specs/stylesheet-v1.md)):

- `min-width`/`max-width`, `min-height`/`max-height`
- `aspect-ratio` and `min-`/`max-aspect-ratio`, compared as an exact integer ratio (issue #691)
- `hover`/`any-hover` and `pointer`/`any-pointer`, answered from the window's input capabilities
  (issue #691); the Studio's device preset supplies them for the simulation
- the range form for width, height, aspect-ratio and resolution: `(width >= 600px)`,
  `(600px <= width)` and the two-sided `(600px <= width <= 900px)` (issue #691)
- `device-posture: continuous|folded` and `horizontal-viewport-segments` / `vertical-viewport-segments`
  (integer, range forms), the W3C names for foldables ([ADR-074](../../decisions/ADR-074-stylesheet-environment-w3c-names-and-tessera-prefix.md)):
  `folded` is a half-open fold, and a *separating* fold makes two segments along the axis its hinge
  crosses. They answer from `FoldInfo` through `with_fold`, in the host and in the preview alike.
  The hinge angle is not a media answer, so a slow fold re-resolves nothing
- `orientation: portrait|landscape`
- `resolution` / `min-resolution`
- `prefers-color-scheme`
- `prefers-reduced-motion`
- `prefers-contrast` (no release assigned; not listed in stylesheet-v1.md)
- `display-mode` (no release assigned; not listed in stylesheet-v1.md)

**Viewport units.** `vw`, `vh`, `vmin`, `vmax` and the small/large/dynamic forms resolve through one
table (`viewport_units.hpp`), so layout, grid and `@media` cannot disagree about what a unit means
(issue #691). The small/large/dynamic variants exist because a mobile browser's viewport changes as
its toolbars slide away; a desktop window has no such chrome, so all three equal the plain value
there. That is the correct answer for such a viewport, not a fallback, and `ViewportUnits` carries a
`has_dynamic_chrome` flag so a platform that can differ has one place to say so.

Breakpoints are app-authored; Tessera does not impose a fixed set. Recommended defaults for
dashboard-style apps: compact < 600 dp, medium 600–839 dp, expanded >= 840 dp. These values
are guidance, not semantics.

### 2.1 The window is the media context (issue #436)

Every `@media width/height/orientation/resolution` answer must be resolved against the **window the
tree is in**, not against a default rectangle. `MediaContext::viewport_width/height` are the logical
client area and `resolution_dpi` is the DPI scale in CSS dpi (96 at scale 1, so a 2x display answers
`(min-resolution: 2dppx)`). Both `AppHost::resize` and a `set_window_metrics` snapshot that names a
different extent move it, and so does a design-time device preset (the preview sessions derive the
same context from the simulated viewport). Before #436 the context kept its 1280x720 defaults, so
`@media (max-width: 600px)` was evaluated for a rectangle no window had: the boxes re-laid out and
the responsive stylesheet did not respond.

**Re-resolve only when an answer can change.** A drag-resize arrives as many size changes per
second, and rebuilding the cascade on each of them throws the paint cache away for nothing. The host
therefore asks the prepared (media-independent) sheet whether the move can flip any `@media` answer:
`PreparedStyleSheet::media_answer_can_change` compares `evaluate(list, context)` - the very boolean
the resolve step uses - for the old and the new context. When none flips, the resize is a **layout
pass and nothing else**; when one does, the cascade is re-resolved from the same prepared data
(ADR-048's resolve step, never a re-parse) in the same pass. The rule is exactly "rebuild when a
breakpoint is crossed", computed without parsing a length, special-casing `orientation` or guessing
at `vw`/`vh`, so it is exact rather than a superset.

`AppHost::cascade_generation()` (`PreviewSession`/`CanvasSceneSession::cascade_generation()` in the
preview) counts the resolutions, which is how the rule is asserted: a resize that crosses no
breakpoint leaves the counter alone while the boxes move. A design-time viewport change goes through
`CanvasSceneSession::set_viewport()`, which re-lays the running tree out in place and re-resolves
`@media` on the same condition - no re-evaluation, so the state cells, hover and focus survive.

`@supports` and `@container` are unaffected: neither is a function of the window rectangle
(container queries are relative to an ancestor's box, section 10).

`@container` size queries are evaluated by the style engine and wired into the runtime (ADR-041
slice 8, issue #693). The shared `ui::layout::LayoutSettle` component owns the layout read model, the
`ContainerQueryTracker` and the bounded settle loop; `AppHost` and the Studio canvas session both
call it, so the preview cannot drift from the runtime. After every layout pass the host publishes the
committed boxes, the tracker compares each query container's content box with the size it was last
styled at, and a container whose `@container` answer changed marks its descendants `Dirty::Style`.
The settle round then re-runs style and layout **in the same tick**, so the frame that paints a
resize already paints the responsive result: there is no frame with the stale layout. A resize that
crosses no condition costs one layout pass and no style work, and a stylesheet without `@container`
pays one boolean test per frame. The round budget is `layoutObservationMaxRounds` (default 4, range
1..16); a rule that flips its own container's size terminates at the limit with
`TESSERA_LAYOUT_OBSERVER_LOOP` and never hangs the frame. Evidence: `layout_settle_host`
(`tests/unit/layout_settle_host_tests.cpp`).

## 3. Size classes

The runtime exposes derived size classes (compact/medium/expanded) as the `useBreakpoint()` hook
value. They are computed from the **window** (not the display) and update on resize/fold/multi-window.
Container size classes have no release assigned (see section 10).

**The thresholds are fixed in V1** (issue #692 decision): `compact` below 600 dp, `medium` from 600
to 839, `expanded` from 840 upward. They are named constants in
`include/tessera/ui/responsive/size_class.hpp` (`kCompactMaxWidth`, `kMediumMaxWidth`), not magic
numbers, because three consumers must agree on them: the hook, the preview's `viewport.set` report,
and any app-authored CSS that writes the same numbers. An app that wants different breakpoints
writes its own `@media` rules; it does not reconfigure the hook. That keeps one answer per question:
a stylesheet breakpoint and the hook can never disagree about where a class starts, because both
read `MediaContext::viewport_width`.

A window that has not been measured yet (a non-finite or negative width) is `compact`, so a
component that branches on the class renders its narrow layout rather than its widest one on the
first frame. Evidence: `size_class` (`tests/unit/size_class_tests.cpp`) asserts both sides of each
boundary, the degenerate widths, and that the media context is the input.

**How the value reaches a component.** `ui::responsive::EnvironmentSet` is the per-window side table
of consumers (a plain function pointer plus a context, so no `std::function` and no RTTI). A
generated component registers its `set_environment_<name>()` at mount and unregisters at unmount,
mirroring `attach_scheduler`. `AppHost::apply_viewport` pushes the class after every size change,
from the same `MediaContext` the `@media` rules resolve against, and returns immediately when the
class did not move: a resize inside one class walks nothing and schedules nothing. The generated
setter is equality-gated, so even a push that reaches a consumer with the value it already holds is
a no-op. Evidence: `breakpoint_host` (`tests/unit/breakpoint_host_tests.cpp`) drives a real
`AppHost` across 600 and 840 in both directions and asserts the pushed sequence, the no-push case,
and that the hook and the stylesheet read the same context.

## 4. Safe areas

- Layout uses safe-area insets for top/bottom/left/right; content never sits under a notch,
  cutout, system bar, or IME unless it opts into edge-to-edge.
- Paint may extend into the safe area for backgrounds; interactive content must not.
- IME insets animate; layout should follow the inset animation without re-creating the tree.
- Golden tests use fixed insets; device tests cover notch/cutout/gesture-bar variants.
- System chrome (title bar, menu bar, status/navigation bars) and its appearance are specified in
  [`window-chrome-v1.md`](../specs/window-chrome-v1.md); this section owns only the content-versus-
  safe-area rule.

## 5. Fold postures

| Fold state | Orientation | Layout |
|---|---|---|
| `FLAT` | any | normal large screen; avoid unnecessary two-pane |
| `HALF_OPENED` | `HORIZONTAL` | tabletop: content above, controls below |
| `HALF_OPENED` | `VERTICAL` | book: left/right logical panes |
| `occlusion=FULL` | any | hinge is not visible/touchable; never place content there |
| `separating=true` | any | treat as two logical areas; split content/controls |

Fold terms (`FLAT`, `HALF_OPENED`, occlusion, separating, hinge bounds) are defined in [`os-events-v1.md`](../specs/os-events-v1.md) (`FoldInfo`, see the glossary).

Rules:

- Use `hinge_bounds` in window coordinates to avoid placing interactive elements on the hinge.
- Preserve selection/scroll/focus across fold/unfold.
- Do not restart animations on every fold transition; retarget at a frame boundary.
- Android may stop/restart the app on fold/unfold; save/restore state for continuity.
- Windows/Linux foldables lack a stable hinge API; degrade to size/DPI/orientation changes.

## 6. Multi-window, PiP, and desktop windowing

- Multi-window: layout adapts to the window bounds, not the display bounds.
- PiP: reduce chrome, pause heavy rendering, keep essential content; respect platform rules.
- Desktop windowing/freeform: resizable windows, minimum sizes, focus/occlusion.
- External displays: separate window/scene; do not assume a single display.
- The app never assumes "phone" or "desktop"; it reacts to metrics.

## 7. Text scale, locale, and accessibility

- Text scale (font scale) reflows layout; do not clip text.
- RTL flips logical start/end; icons with direction are mirrored only when the icon declares `mirror` (see [`component-library.md`](component-library.md) section 8.1).
- High contrast/reduced motion change colors/motion via media queries/inputs.
- Touch targets remain >= platform minimum (44/48 dp, density-independent pixels) after scaling; behavior when scaling makes this impossible is not specified.

## 8. Invalidation

- Size/orientation/safe-area/fold/text-scale/locale changes -> root layout dirty (dirty = needs recomputation, see [`dirty-rendering.md`](dirty-rendering.md)).
- Breakpoint changes -> style dirty for affected rules; layout/paint follow computed changes. A size
  change that crosses no breakpoint is a layout pass only: the cascade keeps its generation and the
  paint cache keeps its records (section 2.1 is the rule, and `AppHost::cascade_generation()` is the
  counter that proves it).
- Fold posture changes -> style/layout dirty (container/query inputs).
- Display/DPI change -> style/layout/paint; target may recreate ([`window-resize.md`](window-resize.md)).
- No full-tree rebuild unless the platform forces a restart; preserve state.

## 9. Testing

- Synthetic metrics scripts: resize, orientation, safe-area/IME, fold flat/half-opened,
  multi-window/PiP, text scale, locale/RTL.
- Golden layouts at compact/medium/expanded widths and at 1.0/1.25/1.5/2.0 scale.
- Fold tests: tabletop/book, hinge occlusion, separating, continuity.
- No device-specific assumptions in unit tests; device smoke tests for real insets/hinges.

## 10. Open questions

- Container queries and their invalidation model. **Resolved** (ADR-041, Accepted 2026-11-11;
  implemented by issue #693): evaluated after layout inside the settle rounds of
  [`layout-observation.md`](layout-observation.md) section 5.4, wired into `AppHost` and the Studio
  canvas session through the shared `ui::layout::LayoutSettle` component (section 2.2).
- Breakpoint tokens in the theme vs app-authored CSS.
- Safe-area behavior for background layers vs content.
- Fold continuity across Android process restart.
- Desktop windowing minimum-size policies.
