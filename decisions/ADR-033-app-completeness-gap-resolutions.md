# ADR-033: App-completeness gap resolutions

- Status: Accepted
- Date: 2026-11-07
- Related: ADR-029, ADR-030, ADR-031, ADR-032

## Context

The app-level specs identified 27 initial P0 gaps and 25 open questions across text, scroll,
overlays, forms, navigation, restore, shell, i18n, accessibility, data, devtools, updates, crash,
library, samples, and media. Animation/overscroll was added as G-28/Q-26 in ADR-034. Leaving gaps
open blocks implementation and invites incompatible choices.

## Decision

Record a decision for every gap/open question, with an explicit deferral where V1 should not
carry the scope. Canonical details live in the specs; the ledger is
`docs/architecture/app-completeness-gap-resolutions-v1.md`.

Key decisions:

1. **Text**: piece table above 64 KiB; editing owns selection handles; platform autofill/
   spellcheck; text/plain clipboard only.
2. **Scroll/gestures**: gesture arena in `ui/input`; HAL reserves system gestures; overlay
   scrollbars/sticky/RTL rules.
3. **Overlays**: one default host per window; modal makes lower layers inert; shared
   `BackDispatcher` order overlay -> modal route -> route stack -> OS.
4. **Forms**: generated field schema; nested paths/keyed arrays; epoch-based validation/submit;
   server error mapping; autofill merge policy.
5. **Navigation/restore**: guards with epoch/timeout/redirect limit; route data in async-data;
   JSON restore snapshots with stable route/uiId/itemKey keys and bounded writes.
6. **Shell**: capability bitmask; file-promise DnD caps; scoped shortcuts and app/global wins.
7. **i18n**: JSON + ICU MessageFormat compiled to C++/binary; static ICU data; bundled locale
   packs; `en-XA`/`ar-XB` pseudo-locales.
8. **Accessibility**: normative role/action matrix; epoch-checked UI-thread actions; deduped
   announcements; CI semantics snapshots + nightly screen-reader smoke.
9. **Data**: canonical keys/scopes; optional encrypted disk cache; bounded offline queue;
   server-wins conflict by default with surfaced errors.
10. **Production**: local JSON-lines devtools with style/structural hot reload; full signed
    updates v1 (delta deferred), health-check/rollback; Crashpad/fallback symbols, opt-in
    upload.
11. **Product layer**: versioned semantic tokens; fixed component API/DataGrid scope; samples
    checked-in and CI-tested; media/print/export optional.

Deferred: rich text/HTML clipboard, streaming/subscriptions, cross-window overlays, per-tab
nested stacks, delta updates, Linux tray/global shortcuts, custom dictionaries, DRM/protected
media, rich PDF/print fidelity.

## Consequences

- Implementation can proceed against concrete contracts.
- Some platform/product scope is explicitly excluded from V1.
- Deferred items have a documented entry point and do not block P0 work.
- The gap ledger must stay in sync with the specs.

## Alternatives

- Leave gaps open and decide per-component: rejected (incompatible boundaries).
- Implement every deferred feature in V1: rejected (scope/risk).
- Put decisions only in the ledger: rejected (specs must remain normative).

## Follow-up

- Add the decisions to each canonical spec.
- Convert each resolved decision into acceptance tests.
- Revisit deferred items only after P0 tests are green.
