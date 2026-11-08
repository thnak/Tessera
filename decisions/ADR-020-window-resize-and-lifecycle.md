# ADR-020: Window resize, DPI, and lifecycle separation

- Status: Proposed
- Date: 2026-11-07
- Related: [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin render hardware interface and batched 2D renderer), [ADR-017](ADR-017-input-and-fixed-step-runtime.md) (input and fixed-step runtime loop), [ADR-018](ADR-018-dirty-rendering-and-frame-pacing.md) (dirty rendering and frame pacing), [ADR-019](ADR-019-power-aware-runtime.md) (power-aware runtime defaults), [ADR-021](ADR-021-async-runtime.md) (async runtime)

## Context

Window resize touches styles, layout, animation, input coordinates, render targets, swapchains,
DPI (dots per inch, the display scale), multi-monitor, and OS-specific lifecycle rules. If components or styles handle OS resize
events directly, the framework becomes platform-dependent, non-deterministic, and full of
re-entrancy bugs (resize -> layout -> render -> resize). If infrastructure hardcodes style
values, the declarative model breaks.

## Decision

1. **Declarative layer** (styles/animations/components) sees immutable `WindowMetrics`
   (logical/physical size, DPI, refresh, safe area, focus/occlusion) through the style
   environment/layout constraints. It never sees OS messages, window handles, or swapchains.
2. **Infrastructure** (HAL (hardware/OS abstraction layer)/runtime/renderer) owns platform events, coalescing, DPI, viewport/
   media/container invalidation, layout invalidation, swapchain/target recreation, resource
   retirement, and present-mode re-query.
3. Resize is consumed at a frame boundary in this order:
   `normalize -> coalesce -> style environment -> style -> layout -> paint -> target -> render`.
   The platform callback records only.
4. Support `live`, `deferred`, and `adaptive` resize modes. Default `adaptive`; no component
   branches on the mode. Debounce and swapchain-recreate delay are runtime knobs.
5. Keep the old swapchain/render target until the new one is ready and in-flight frames using
   the old one are retired. Never destroy in-use resources. Handle out-of-date/suboptimal,
   device loss, OOM (out of memory), and zero-size/minimized without crashing.
6. Transitions/keyframes retarget to the new computed values; they do not restart on every
   resize event. Animation never drives layout size (no feedback loop).
7. OS compliance:
   - Windows: `WM_SIZE`/`WM_DPICHANGED`/`WM_ENTERSIZEMOVE`/`WM_EXITSIZEMOVE`, per-monitor DPI v2,
     flip-model/waitable swapchain, DXGI error handling, no blocking message loop.
   - X11: `ConfigureNotify`, RandR, `_NET_WM_SYNC_REQUEST` where the window manager supports it, ICCCM/EWMH hints,
     no blocking round-trips.
   - Wayland: `xdg_surface.configure`/`ack_configure` handshake, `xdg_toplevel` states,
     `wl_output.scale`/`wp_fractional_scale_v1`/`wp_viewporter`, `wl_surface.frame` and
     `wp_presentation`; never draw before ack; compositor owns size.
   - macOS/Android: not specified in this ADR (macOS is a [V1](../docs/reference/glossary.md) non-goal, [ADR-010](ADR-010-macos-via-moltenvk-deferred.md) (macOS via MoltenVK, deferred from the first release); Android events are covered by [ADR-023](ADR-023-os-event-normalization-and-foldables.md) (OS event normalization and foldables)); keep the normalized metrics contract.
8. Resize/DPI/theme/display changes are deterministic synthetic inputs for tests; [golden](../docs/reference/glossary.md) tests
   never depend on OS event cadence.
9. Multi-monitor: per-monitor DPI/scale/refresh; re-query present/VRR on display change.
10. No UI-thread blocking on the compositor/GPU; acquire/present/recreate use bounded state
    machines ([ADR-021](ADR-021-async-runtime.md)).

## Consequences

### Positive

- Components/styles remain platform-neutral and deterministic.
- Resize handling is centralized and testable.
- No re-entrancy or use-after-free around swapchain recreation.
- Correct DPI/scale behavior across platforms.

### Negative / risks

- Platform behavior varies (Windows modal loop vs Wayland configure/ack).
- Coalescing/debouncing heuristics need tuning for slow scenes.
- Old-swapchain retirement adds lifetime complexity.
- Test coverage requires per-platform integration tests.

## Alternatives considered

- **Let styles/components observe OS resize events.** Rejected: platform leakage, non-
  deterministic, re-entrancy.
- **Recreate the swapchain on every size event.** Rejected: thrashes the GPU, blocks the loop,
  breaks live resize.
- **Use GLFW/SDL3 as the window abstraction.** Kept as a possible HAL backend, but they do not
  remove the need for swapchain/DPI/OS-compliance policy in the renderer/runtime.
- **Custom window frames everywhere.** Rejected: fights the compositor and accessibility; use
  server-side decorations (window frames drawn by the compositor) on platforms that offer them.

## Follow-up

- Implement the normalized `WindowMetrics`/resize state machine and synthetic tests.
- Add Windows/X11/Wayland backends and platform-specific integration tests.
- Implement old-swapchain retirement and device-loss recovery.
- Document resize modes/defaults in [`docs/specs/runtime-config-v1.md`](../docs/specs/runtime-config-v1.md).
