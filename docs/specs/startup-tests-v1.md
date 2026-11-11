# Startup tests and acceptance v1

Status: proposed; normative companion of [startup-api-v1.md](startup-api-v1.md).
Decision: [ADR-042](../../decisions/ADR-042-jsx-startup-surface.md),
[ADR-043](../../decisions/ADR-043-launch-modes-and-state-reuse.md).
This file owns the acceptance tests of the startup surface, including launch modes and the stored
state it reads; the surface rules themselves stay in [startup-api-v1.md](startup-api-v1.md) and the
launch-mode rules in its section 5.1.

Chrome-specific acceptance tests live in [window-chrome-v1.md](window-chrome-v1.md) section 11.

## 1. Compile time

- Accepted-form fixtures for startup-api-v1 sections 3 and 4, with an IR golden containing the
  hoisted startup record and no declaration nodes.
- One rejection fixture per code of startup-api-v1 section 9 (code, span, message) and the
  diagnostics registry golden.
- Determinism: the same source and config produce byte-identical IR, manifest and diagnostics.
- Variants: nearest-scale selection, duplicate `scale`, a variant that contradicts the primary's
  priority (`TESSERA_JSX_STARTUP_VARIANT`), and an uncovered placeholder
  (`TESSERA_JSX_STARTUP_PLACEHOLDER`).
- Route scope: an unknown `route` id is `TESSERA_JSX_STARTUP_ROUTE`; a known route scopes the
  resource id deterministically.
- Brand: `loading.brand` logo not in `assets.images`, or an invalid background, is
  `TESSERA_JSX_STARTUP_BRAND`.
- Deprecated `loading.screen`: warning when used, and the JSX loading subtree wins when both exist.

## 2. Runtime (headless, deterministic; `ManualClock` + CPU sink)

- Phase order: P2 presented before any critical resource completes; the app UI is not built before
  P3; P4 after the critical set and restore; P5 only after P4.
- No blocking: no tick exceeds the per-tick budget; completions apply at frame boundaries.
- First-frame and default-screen goldens at two window sizes, two DPI scales, light/dark theme and
  reduced motion.
- Brand override: the default screen uses the configured logo/background, and falls back to the
  framework mark and `--window-background` without it.
- Failure: critical failure -> retry -> root fallback; never blank, never an endless spinner; a
  custom loading failure -> default screen plus diagnostic.
- Cancellation/close during P3/P5; P5 pauses in the background; a static scene presents nothing
  after settle.
- Accessibility: the default screen exposes the progressbar name; a custom subtree without
  semantics gets the wrapper.
- Mobile: generated splash theme/launch-screen artifact golden and P0 -> P2 continuity.
- Route scope: a route-scoped `critical` resource renders at P2 when that route is the
  initial/activation target; an unresolved route reports `TESSERA_STARTUP_LOADING_ROUTE` and renders
  the placeholder or nothing.

## 3. Launch modes and stored state

Each launch mode is forced deterministically (framework-internal detection inputs) and asserted:

- cold, warm (reuse device/caches, no P0), hot resume (no phase, no loading screen, no presents
  until wake after settle) and process restore (a valid snapshot is applied; a corrupt, foreign or
  undecryptable snapshot is discarded).
- Deep-link and notification launches on cold/warm/hot: the target overrides the restored route,
  survives auth, dedupes, and an invalid target falls back.
- Activation queue: four entries, FIFO routing at stage boundaries, dedupe by normalized target,
  overflow drops the oldest with `TESSERA_STARTUP_ACTIVATION_DROPPED`, logout clears the queue.
- Hot resume: a non-auth screen presents the retained frame before revalidation; an auth-gated
  screen refreshes before its first present.
- Snapshot encryption: a key derived from OS secure storage is required; a missing key or a failed
  decryption discards the snapshot; logout deletes the snapshot and the key material.
- Reuse invalidation: an app-version, route-table, asset-identity or signed-in-user change
  downgrades reuse (`hot_resume` -> `warm` -> `cold`) or discards the snapshot (G17).
