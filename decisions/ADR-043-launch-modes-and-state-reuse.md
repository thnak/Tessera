# ADR-043: Launch modes and state reuse

- Status: Proposed
- Date: 2026-11-11
- Related: [`startup-api-v1.md`](../docs/specs/startup-api-v1.md) (normative behavior and matrix),
  [`state-restoration-v1.md`](../docs/specs/state-restoration-v1.md) (snapshot, restore order),
  [`deep-linking-v1.md`](../docs/specs/deep-linking-v1.md) (activation event/routing),
  [`services.md`](../docs/design/services.md) section 6 (notifications),
  [ADR-019](ADR-019-power-aware-runtime.md) (power/idle), [ADR-020](ADR-020-window-resize-and-lifecycle.md)
  (lifecycle), [ADR-021](ADR-021-async-runtime.md) (async/no blocking),
  [ADR-042](ADR-042-jsx-startup-surface.md) (startup surface).
- Documents: [`os-events-v1.md`](../docs/specs/os-events-v1.md) (normalized lifecycle events).

## Context

The repository's startup documents treat cold, warm and hot starts qualitatively
([`startup-and-loading.md`](../docs/design/startup-and-loading.md) section 2): cold pays process/device
setup, warm has caches, hot must "avoid re-running startup work". That is not operational enough to
build against. Four questions recur, and each has a different answer depending on *why* the app is
starting:

1. Must the framework create a device/surface, and may it reuse caches? (cold vs warm)
2. Must it run any startup phase at all when the user returns to a still-valid app? (hot resume)
3. Must it rebuild UI state from disk after the OS killed the process? (process restore)
4. Must it honor an external target that caused the launch, and does that target outrank restored
   continuity? (deep link / notification)

Today an app would answer these itself with lifecycle flags and ad-hoc booleans, which breaks the
non-negotiables: it would re-run init on foreground (wasted budget, jank), flash stale user data to
the wrong account (privacy), lose a deep link behind the startup/auth gate (UX), or trust a snapshot
blob (security). The framework owns the window, lifecycle, resource and restore layers, so it is the
only place that can decide reuse safely and consistently across platforms.

## Decision

1. **Two orthogonal axes, framework-determined.** The launch mode is `cold`, `warm`,
   `hot_resume` or `process_restore`; the activation is `none`, `deep_link` or `notification`. The
   six user-facing scenarios are combinations, not a single enum. The app declaration cannot select
   either (no mode prop); environment and OS state decide. Tests and the headless harness may force
   a mode.
2. **Per-mode minimum work and a reuse matrix.** Cold runs P0-P5; warm skips P0, recreates only the
   destroyed surface/tree and reuses device/pipelines/caches; hot resume runs no startup phase, shows
   no loading screen and rebuilds nothing; process restore is cold plus a validated snapshot. The
   matrix is normative in the startup spec (section 5.1), not re-derived per subsystem.
3. **Activation outranks continuity.** A pending deep-link/notification target overrides the
   restored route; without an activation a valid snapshot overrides the default UI. Auth-gated
   targets wait for auth and survive login; destructive actions still require confirmation; invalid
   targets fall back to the not-found route or route root. Pending activations are bounded (V1: one,
   latest wins) and deduped.
4. **Untrusted persistence.** A snapshot is validated (version, route table, limits, ids), migrated,
   and never executed; corrupt/foreign snapshots fall back to the previous good snapshot or the route
   root. User-scoped restore and user-scoped activations apply only after auth; logout clears both.
   Secrets are never restored (they live in the keystore).
5. **Reuse must expire.** A change of app version, route table, asset identity or signed-in user
   downgrades reuse (`hot_resume` -> `warm` -> `cold`) or discards the snapshot; hot resume always
   revalidates session and stale data in the background instead of trusting the last frame's data.
6. **Launch is observable.** Every launch records `launch.mode` and `activation.source` as
   diagnostics and metric labels, with `startupMs`, `ttfpMs`, `ttiMs`, `resumeMs` per mode. Forcing
   a mode is a harness capability, never a release feature.
7. **Developer surface stays declarative.** `App restoreState` remains the only startup switch
   (restore on/off). Launch info reaches app code through a future hook (`useLaunchMode()`), which
   depends on the hook manifest slice, and through C++ host APIs; no new JSX props are introduced
   for modes or activations.

## Consequences

- The runtime needs a `LaunchMode`/`ActivationSource` seam in `AppHost`, fed by normalized OS
  lifecycle events (`os-events-v1.md`) and the activation queue. Until that slice lands, the mode is
  always treated as cold.
- State restoration stops being a post-mount convenience and becomes a startup input: the snapshot
  read/validate step must run during P3 and its result feeds route resolution before P4. The
  `RestoreService` slice and startup slice therefore have an ordering dependency.
- Deep-link and notification launches become first-class cold paths: adapters must be able to hand a
  validated activation to the startup sequence before the first screen, not after the app shell.
- Warm start requires the platform layer to keep GPU device/caches alive after the last window
  closes, or to recreate only the surface. That is a platform capability, not free reuse.
- Tests must exist per mode with forced inputs; "everyone tests cold start only" becomes a reviewable
  gap. Golden images differ per mode (hot resume has no loading frame).
- Metrics gain a launch dimension, which changes dashboards and budget checks; budgets are per mode
  (hot resume targets one frame to first present).

## Alternatives

- **Always behave as cold start.** Simplest, and it is the current implicit behavior. Rejected: it
  re-runs device/font/atlas/pipeline work on every foreground return, contradicts ADR-019
  (power-aware) and the loading matrix, and cannot meet the warm-start budget.
- **App-declared launch mode** (a prop or config flag). Rejected: the environment decides whether the
  process survived, the OS killed it, or a link launched it; an app cannot know reliably, and a
  wrong declaration would skip initialization or trust stale state.
- **One combined "resume" mode** covering warm, hot and process restore. Rejected: they differ in
  what is reused (device/caches/tree) and in what must be validated (snapshot, auth), so one set of
  guarantees would be the weakest of the three.
- **Treat deep links/notifications as post-start navigation only.** Rejected: the deep-linking spec
  already promises cold-start parsing and auth-pending activation; deferring them would regress that
  contract and lose the target behind the loading screen.
- **Restore everything, including secure fields and in-memory async work.** Rejected: violates the
  state-restoration security rules; secrets go to the keystore, secure fields are excluded, and
  in-memory work may not have completed.
- **Restore in-memory caches by serializing them.** Rejected for V1: the caches are bounded GPU/CPU
  state with no stable format; process restore rebuilds them from disk/network instead.

## Follow-up

- Slice S7 in [`startup-api-v1.md`](../docs/specs/startup-api-v1.md) section 12: detection, warm/hot
  resume, process restore, activation routing and precedence, with the mode-matrix tests.
- Wire the OS lifecycle events (`os-events-v1.md`) and the `RestoreService` snapshot read into the
  P3 step; wire the deep-link/notification adapters into the activation queue before P2.
- Add the `LaunchMode`/`ActivationSource` values to the diagnostics model and the audit report keys.
- Revisit the one-pending-activation policy and per-mode budgets when the deep-linking open
  questions (pending queue policy) are decided.
