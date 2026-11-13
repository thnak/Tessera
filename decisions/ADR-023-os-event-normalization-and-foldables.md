# ADR-023: OS event normalization and foldable metrics

- Status: Accepted (2026-11-13, owner)
- Date: 2026-11-07
- Related: [ADR-017](ADR-017-input-and-fixed-step-runtime.md) (input and fixed-step runtime loop), [ADR-020](ADR-020-window-resize-and-lifecycle.md) (window resize, display scale, and lifecycle), [ADR-021](ADR-021-async-runtime.md) (async runtime), [ADR-022](ADR-022-diagnostics-and-profiling.md) (diagnostics and profiling)

## Context

Windows, X11, Wayland, Android, iOS, and macOS expose different window, display, power, and
input events. Foldables add fold posture, hinge occlusion, separating features, and app
continuity. If platform events leak into styles/animations/components, the framework becomes
platform-dependent and non-deterministic. If the HAL (hardware/OS abstraction layer) only models "resize", mobile orientation,
insets, fold posture, multi-window, HDR (high dynamic range), and VRR (variable refresh rate) are lost.

## Decision

1. The HAL/runtime normalizes platform events into `OsEvent`, `WindowMetrics`, and `FoldInfo`.
   Styles/animations/components never see window handles, OS messages, or platform structs.
2. The normalized taxonomy and per-platform source mapping are normative in
   [`docs/specs/os-events-v1.md`](../docs/specs/os-events-v1.md).
3. Foldables are modeled as **display features**, not a separate UI mode:
   `state` (`FLAT`/`HALF_OPENED`), `orientation` (`HORIZONTAL`/`VERTICAL`), `occlusion`
   (`NONE`/`FULL`), `separating`, and `hinge_bounds`. Android uses Jetpack WindowManager
   `WindowInfoTracker`/`FoldingFeature`; Windows/Linux fall back to display topology/DPI/size
   because they lack a stable cross-vendor hinge API.
4. Display capabilities are first-class metrics: DPI (dots per inch, the display scale)/scale, refresh/VRR, HDR/color gamut,
   safe/work areas, multi-window/PiP, orientation.
5. Events are coalesced and consumed at a frame/tick (simulation step, see [ADR-017](ADR-017-input-and-fixed-step-runtime.md)) boundary; lifecycle/fold/power transitions
   are never dropped. Ordering is stable and deterministic.
6. Platform callbacks only record events: no layout/render/user code, no blocking waits.
   Resize/swapchain follow [ADR-020](ADR-020-window-resize-and-lifecycle.md); async completions follow [ADR-021](ADR-021-async-runtime.md).
7. Unknown/unsupported events are ignored with diagnostics and documented fallbacks; never a
   crash. Missing refresh/VRR/scale assumes 60 Hz/1.0 scale and records the assumption.
8. Tests use synthetic/recorded event scripts per platform, including fold sequences,
   resize/DPI storms, display hotplug, suspend/resume, session lock, and power/thermal changes.
   [Golden](../docs/reference/glossary.md) mode never calls platform APIs.

## Consequences

### Positive

- One event model across desktop, mobile, and foldables.
- Deterministic tests for platform-specific behavior.
- Fold/two-pane/tabletop layouts are possible without platform leakage.
- Display/refresh/color capabilities are handled consistently.

### Negative / risks

- Per-platform mapping/maintenance is substantial.
- Foldable vendor behavior (hinge APIs, app continuity) varies.
- Certain events are hints or are coalesced by the compositor, so exact semantics differ per platform (the per-event mapping is in [`docs/specs/os-events-v1.md`](../docs/specs/os-events-v1.md)).
- Windows/Linux foldables lack a standard hinge API; degrade to topology/size changes.

## Alternatives considered

- **Expose raw platform events to UI code.** Rejected: platform leakage and non-determinism.
- **Model foldables as a size/orientation change only.** Rejected: loses posture/hinge/occlusion,
  causes broken two-pane layouts.
- **Use a windowing library (GLFW/SDL3) only.** Rejected as the sole solution: it does not cover
  fold posture, display capabilities, power, or platform lifecycle compliance.
- **Separate UI mode per form factor.** Rejected: duplicates layouts; adaptive metrics are better.

## Follow-up

- Implement the normalized event structs and synthetic event injector.
- Add Windows/X11/Wayland/Android backends, then iOS/macOS when those targets land.
- Add fold/posture layout tests and app-continuity restore tests.
- Keep [`docs/specs/os-events-v1.md`](../docs/specs/os-events-v1.md), [`docs/design/window-resize.md`](../docs/design/window-resize.md), and runtime metrics in sync.
