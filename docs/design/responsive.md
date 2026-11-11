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
- `orientation: portrait|landscape`
- `resolution` / `min-resolution`
- `prefers-color-scheme`
- `prefers-reduced-motion`
- `prefers-contrast` (no release assigned; not listed in stylesheet-v1.md)
- `display-mode` (no release assigned; not listed in stylesheet-v1.md)

Breakpoints are app-authored; Tessera does not impose a fixed set. Recommended defaults for
dashboard-style apps: compact < 600 dp, medium 600–839 dp, expanded >= 840 dp. These values
are guidance, not semantics.

`@container` size queries are evaluated by the style engine (ADR-041 slice 8; the host settle step that re-resolves on a size change is not wired yet). Until a host wires it, use size classes derived
from metrics or explicit props.

## 3. Size classes

The runtime may expose derived size classes (compact/medium/expanded) as custom properties or
media-query-like inputs. They are computed from the **window** (not the display) and update on
resize/fold/multi-window. Container size classes have no release assigned (see section 10).

## 4. Safe areas

- Layout uses safe-area insets for top/bottom/left/right; content never sits under a notch,
  cutout, system bar, or IME unless it opts into edge-to-edge.
- Paint may extend into the safe area for backgrounds; interactive content must not.
- IME insets animate; layout should follow the inset animation without re-creating the tree.
- Golden tests use fixed insets; device tests cover notch/cutout/gesture-bar variants.

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
- Breakpoint changes -> style dirty for affected rules; layout/paint follow computed changes.
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

- Container queries and their invalidation model. Proposed: evaluated after layout inside the
  settle rounds of [`layout-observation.md`](layout-observation.md) section 5.4 (ADR-041, Proposed).
- Breakpoint tokens in the theme vs app-authored CSS.
- Safe-area behavior for background layers vs content.
- Fold continuity across Android process restart.
- Desktop windowing minimum-size policies.
