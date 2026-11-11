# Startup, page load, loading screens, and authentication

Status: design for Big Step B2 (runtime slice) and Big Step F (production breadth); both are defined in
[AGENTS.md Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step). Native app startup/screen
load, not web page navigation. HAL (hardware/OS abstraction layer) is described in [`hal.md`](hal.md);
other terms are in [`glossary.md`](../reference/glossary.md).

The developer-facing startup surface — the JSX `App` root, the replaceable framework loading screen,
prioritized resource hints, launch modes and the P0-P5 phase guarantees — is normative in
[`startup-api-v1.md`](../specs/startup-api-v1.md); this document keeps the UX rationale, loading
modes and budgets.

This document covers cold/warm startup, first screen, loading UI, auth gates, route/screen
load, and the lazy/preload/unload policy. Resource mechanics are normative in
[`resource-lifecycle-v1.md`](../specs/resource-lifecycle-v1.md); the developer decision matrix
is in [`loading-matrix.md`](../guides/loading-matrix.md). Route/auth/restore:
[`navigation-v1.md`](../specs/navigation-v1.md) and
[`state-restoration-v1.md`](../specs/state-restoration-v1.md). The window chrome and system bars
around the loading screen are specified in
[`window-chrome-v1.md`](../specs/window-chrome-v1.md): the launch artifact, the loading screen and
the platform chrome read one resolved `{background, foreground, scheme}` record.

## 1. Principles

1. **Lazy by default.** Only a minimal critical set is eager; everything else is demand-driven.
2. **Preload is scheduled lazy.** Preload is a hint with priority/deadline, not an eager global
   load.
3. **Unload is a feature.** Resources are released on route exit, background, logout, memory
   pressure, or budget eviction.
4. **Never block the UI thread** ([ADR-021](../../decisions/ADR-021-async-runtime.md), async runtime; see
   [`async.md`](async.md)). Auth, I/O, decode, GPU upload, and network are async.
5. **Show something useful early.** The shell/skeleton should appear before data is complete.
6. **No fake progress and no infinite spinner.** Show real stages or an indeterminate state
   with cancel/retry/offline paths.
7. **Measure.** Startup, auth, first screen, and resource wait times are explicit budgets.

## 2. Startup phases

```text
process start
  -> loader/runtime init
  -> config + diagnostics + crash handler
  -> HAL/window/surface
  -> GPU device/queues
  -> critical theme/fonts/icons
  -> session restore / auth check
  -> route/deep-link resolution
  -> first frame (shell/skeleton/loading)
  -> data + non-critical resources
  -> interactive
```

| Phase | Must be | May be lazy |
|---|---|---|
| Loader/runtime | yes | — |
| Config/diagnostics/crash | yes (small) | heavy sinks/tracing |
| Window/surface | yes | — |
| GPU device | yes | pipelines/shaders until first draw |
| Theme/fonts/icons | critical subset | non-critical weights/glyphs |
| Session/auth | check/restore | profile/data after auth |
| Route/deep link | resolve | route bundle/data |
| First frame | yes | full content |
| Data/resources | shell/critical data | images/charts/media/next routes |
| Interactive | as soon as input is handled without blocking the UI thread | background prefetch |

Cold vs warm vs hot is not a single ladder; the launch mode and the activation are two separate
axes, and both are framework-detected:

- **Launch modes** — `cold` (process start), `warm` (process alive, window/surface/tree recreated,
  caches reused), `hot_resume` (tree/resources still valid, foreground return, no startup phase) and
  `process_restore` (OS-killed process resumed from a validated snapshot).
- **Activations** — `none`, `deep_link`, or `notification`; a pending activation target overrides the
  restored route and survives readiness/auth.

The normative per-mode behavior, reuse matrix and precedence are in
[`startup-api-v1.md`](../specs/startup-api-v1.md) section 5.1. In short: cold pays full
initialization; warm recreates only what was destroyed and reuses caches/device; hot resume runs no
startup phase and shows no loading screen; process restore is a cold start plus a validated snapshot.
This document keeps the UX policy, loading modes and budgets.

## 3. Budgets and metrics

Metrics:

- cold/warm start to first frame
- time to first paint (TTFP) / first meaningful screen
- time to interactive (TTI)
- auth check/refresh time
- route/screen load time
- resource wait time and cache hit/miss
- jank/dropped frames during load
- memory peak and network bytes
- unload/reload counts

Budgets are per app profile (see [`loading-matrix.md`](../guides/loading-matrix.md)). Example
targets for a desktop dashboard: first frame < 300 ms warm, < 1 s cold; auth check < 150 ms
cached; route shell < 100 ms; data can stream. Mobile budgets are stricter (cold start, memory,
thermal); measure on real devices.

## 4. Loading screen modes

| Mode | Use | Trade-off |
|---|---|---|
| OS splash | unavoidable on mobile; first frame | no app content; avoid double splash |
| In-app shell | show chrome/nav immediately | needs cached shell/theme |
| Skeleton | known layout, data pending | layout must be stable; accessibility labels |
| Progress | measurable stages | only real progress; no fake % |
| Progressive | render sections as ready | complexity; avoid layout shift |
| Blocking spinner | only when nothing useful can render | worst perceived performance |

Rules:

- First frame should be as early as possible; don't wait for all resources.
- Don't show two splash/loading screens back-to-back.
- Skeletons must match final layout to avoid layout shift.
- Do not fake progress; if unknown, use indeterminate + stage text.
- Always provide cancel/retry/offline/error paths for long loads.
- Minimum display time is a last resort; it can make startup feel slower (the policy is an open question, section 11).
- Announce loading state to accessibility; respect reduced motion.
- Loading UI must not block input/focus unnecessarily.

## 5. Authentication screen

Auth is a startup **state**, not a separate app.

Modes:

| Mode | Behavior | Use |
|---|---|---|
| Blocking gate | no app shell until auth | sensitive apps |
| Shell + auth overlay | show shell, gate actions | lower-friction apps |
| Deferred/guest | guest shell, auth on demand | content-first apps |
| Background refresh | valid session; refresh silently | returning users |

Rules:

- Check/restore session asynchronously; never block the UI thread.
- Don't render user-scoped data before auth is confirmed; don't flash stale data.
- Store tokens in secure storage; never in logs/project config.
- Support refresh, expiry, offline, cancellation, timeout, and retry.
- After auth, continue the pending route/deep link; don't drop it. See
  [`deep-linking-v1.md`](../specs/deep-linking-v1.md).
- On logout: clear user-scoped caches/resources, cancel tasks, return to auth/guest.
- Protect sensitive screens from capture (`FLAG_SECURE`/platform equivalents).
- Auth screen must be accessible: labels, focus order, errors, progress, reduced motion.
- Biometrics/passkeys are platform bridges (OS-edge adapters, see [`bridges.md`](bridges.md)); failure falls back to the normal flow.

## 6. Screen/route load

- Route code/data/resources are lazy by default.
- Show the route shell first; load data/resources progressively.
- Cancel route loads when navigation changes; don't update a stale route.
- Preload the next route on intent (hover/focus/press/back-stack restore), bounded and
  cancellable.
- Preserve scroll/focus/selection on back; restore state from a keyed store.
- Avoid full-screen spinner for a route that has a stable shell/skeleton.
- Virtualize large lists/grids; don't realize offscreen items.

## 7. Resource policy

Default: **lazy**. The critical set is explicit and small:

- bootstrap config/theme tokens
- critical font subset + icons for first screen
- first-screen shell and above-the-fold assets
- auth/session state (small)
- crash/diagnostics bootstrap

Everything else is requested on demand or preloaded with a priority. See
[`resource-lifecycle-v1.md`](../specs/resource-lifecycle-v1.md).

Unload triggers:

- route exit / no lease
- app background/minimized (per the unload policy in [`resource-lifecycle-v1.md`](../specs/resource-lifecycle-v1.md))
- logout (user-scoped resources)
- memory pressure / cache budget
- hot reload (design-time preview, see [`designer.md`](designer.md))/asset generation change
- error/expired resource

Never unload pinned/in-use resources; retire GPU resources after frames in flight.

## 8. Failure, offline, degraded

- Cache-first shell + stale data (only for data the app marks as cacheable; the marking rule is not specified here); mark stale.
- Retry with backoff for transient failures; no retry storm.
- Offline screen with cached content and a clear action.
- Partial UI is better than a blank screen; show what is available.
- Auth failure is distinct from network failure; don't loop the login screen.
- Timeouts are bounded; cancellation is supported.
- Errors are actionable and accessible; no raw OS/HTTP codes in the primary message.
- Install a **root error boundary** so a startup/auth/first-screen failure shows a safe fallback
  instead of a blank window; see [`error-boundaries-v1.md`](../specs/error-boundaries-v1.md).

## 9. Platform notes

- Desktop: first frame can be immediate; GPU/pipeline warm-up may dominate cold start.
- Mobile: cold start includes OS splash/scene; no background rendering; auth/secure storage
  bridges; respect thermal/memory.
- Kiosk/embedded (no platform backend is scheduled; not specified beyond this line): preload critical shell, cap memory, no auth or device auth, unload offscreen.
- Web/Wasm designer (design-time viewer, [`designer.md`](designer.md)): not production; same principles, browser fetch/caching is separate.

## 10. Tests and metrics

- Startup scripts measure cold/warm first frame, TTFP, TTI, auth time, route load.
- Synthetic slow network/disk/GPU; cancellation; offline; auth failure/expiry.
- Memory pressure: unload caches, no crash, no reload storm.
- Deterministic/golden mode pins resources and load order; no network/system fonts.
- Accessibility: loading/auth screens have labels/focus/announcements.
- No UI-thread blocking: load/auth/upload run async; completions at frame boundary.

## 11. Open questions

- Exact startup budgets per platform/form factor.
- Whether auth gate is app-level or route-level by default.
- Minimum loading-screen time policy (no decision yet; section 4 says it is a last resort).
- Preload triggers and budgets on mobile/power-save.
- Cache-first vs network-first defaults per app profile.
- How to measure perceived load time reliably.
- Disk cache encryption/eviction policy.
