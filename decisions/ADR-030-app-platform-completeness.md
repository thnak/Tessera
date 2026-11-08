# ADR-030: App-platform completeness: navigation, restore, shell, i18n, accessibility, data

- Status: Accepted
- Date: 2026-11-07
- Related: [ADR-020](ADR-020-window-resize-and-lifecycle.md) (window resize, display scale, and lifecycle), [ADR-022](ADR-022-diagnostics-and-profiling.md) (diagnostics and profiling), [ADR-023](ADR-023-os-event-normalization-and-foldables.md) (OS event normalization and foldables), [ADR-026](ADR-026-deep-links-and-external-activation.md) (deep links and external activation), [ADR-029](ADR-029-interaction-completeness.md) (interaction completeness)

## Context

Rendering and widgets are not enough for an app: routes/back stacks, process-death restoration,
OS shell integration, localization, accessibility, and async/offline data determine whether the
framework is usable in production.

## Decision

1. **Navigation** is declarative: static route table, typed params, push/pop/replace/reset,
   guards/auth, modal routes, platform back, deep links, and route-level loading/error.
2. **State restoration** is explicit/versioned/bounded: route stack, window geometry, opt-in
   per-route state/scroll/focus/drafts; secure/secret state excluded; atomic writes and safe
   fallbacks.
3. **Platform shell** is a service: clipboard, internal/external drag-drop, context menus, file
   dialogs, menu bar/tray, shortcuts. Async, permission-aware, capability handles, no OS types
   in UI.
4. **i18n (internationalization)** is runtime behavior: locale negotiation, ICU (International Components for Unicode) catalogs/plurals, number/date/currency,
   RTL/bidi, font fallback, text expansion, pseudo-localization, compile-time catalog validation.
5. **Accessibility** is normative: semantics tree, roles/states/actions, focus/keyboard,
   text editing, overlays/navigation, virtualized/custom elements, announcements, platform
   bridges and tests.
6. **Async data** owns keys/cache/status/invalidation/retry/offline/dedupe, with explicit
   loading/refresh/error/empty UI and no hidden fetch in render.
7. All integrate with existing numeric IDs, diagnostics/tracing, headless automation, and
   security/redaction.

## Consequences

- Apps can navigate, restore, localize, expose accessibility, and work offline without custom
  infrastructure.
- Additional services/platform bridges and test matrices.
- Restoration/i18n/accessibility become compatibility surfaces (versioned).
- Cache/offline conflict policy needs explicit app choices.

## Alternatives

- Leave routing/restore/shell/i18n/accessibility/data to apps: rejected (fragmentation).
- Browser-history/router runtime or WebView: rejected (JS/bridge/native-first).
- Save/restore the heap: rejected (unsafe/unversioned/corruptible).
- Machine-translate at runtime: rejected (quality/privacy/determinism).

## Follow-up

- Implement route table/stack/guards/deep-link integration.
- Implement versioned restoration and multi-window snapshots.
- Implement shell services per platform with fake/headless tests.
- Adopt ICU catalogs and pseudo-locale CI (continuous integration).
- Complete semantics/actions/announcements and platform bridge tests.
- Implement resource cache/query/mutation/offline queue.
