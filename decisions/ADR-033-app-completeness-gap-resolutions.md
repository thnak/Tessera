# ADR-033: App-completeness gap resolutions

- Status: Accepted
- Date: 2026-11-07
- Related: [ADR-029](ADR-029-interaction-completeness.md) (interaction completeness), [ADR-030](ADR-030-app-platform-completeness.md) (app-platform completeness), [ADR-031](ADR-031-production-tooling-and-distribution.md) (production tooling and distribution), [ADR-032](ADR-032-bundled-component-library.md) (bundled component library)

## Context

The app-level specs identified 27 initial P0 gaps (G-01..G-27, designs required before implementation; see [`docs/architecture/app-completeness-cross-check-v1.md`](../docs/architecture/app-completeness-cross-check-v1.md) section 3.2) and 25 open questions (Q-01..Q-25, section 4 of that file) across text, scroll,
overlays, forms, navigation, restore, shell, i18n (internationalization), accessibility, data, devtools, updates, crash,
library, samples, and media. Animation/overscroll was added as G-28/Q-26 in [ADR-034](ADR-034-interaction-and-overscroll-animation.md) (interaction and overscroll animation). Leaving gaps
open blocks implementation and invites incompatible choices.

## Decision

Record a decision for every gap/open question, with an explicit deferral where V1 does not
carry the scope (a deferral means out of V1, with a reason and an entry point; see section 1 of the ledger linked below). Canonical details live in the specs; the ledger is
[`docs/architecture/app-completeness-gap-resolutions-v1.md`](../docs/architecture/app-completeness-gap-resolutions-v1.md).

Key decisions:

1. **Text**: piece table (a text-buffer data structure) above 64 KiB; editing owns selection handles; platform autofill/
   spellcheck; text/plain clipboard only.
2. **Scroll/gestures**: gesture arena in `ui/input`; HAL (hardware/OS abstraction layer) reserves system gestures; overlay
   scrollbars/sticky/RTL rules.
3. **Overlays**: one default host per window; modal makes lower layers inert; shared
   `BackDispatcher` order overlay -> modal route -> route stack -> OS.
4. **Forms**: generated field schema; nested paths/keyed arrays; epoch-based validation/submit;
   server error mapping; autofill merge policy.
5. **Navigation/restore**: guards with epoch/timeout/redirect limit; route data in async-data;
   JSON restore snapshots with stable route/uiId/itemKey keys and bounded writes.
6. **Shell**: capability bitmask; file-promise DnD (drag and drop) caps; scoped shortcuts and app/global wins.
7. **i18n**: JSON + ICU (International Components for Unicode) MessageFormat compiled to C++/binary; static ICU data; bundled locale
   packs; `en-XA`/`ar-XB` pseudo-locales.
8. **Accessibility**: normative role/action matrix; epoch-checked UI-thread actions; deduped
   announcements; CI (continuous integration) semantics snapshots + nightly screen-reader smoke.
9. **Data**: canonical keys/scopes; optional encrypted disk cache; bounded offline queue;
   server-wins conflict by default with surfaced errors.
10. **Production**: local JSON-lines devtools with style/structural hot reload; full signed
    updates v1 (delta deferred), health-check/rollback; Crashpad/fallback symbols, opt-in
    upload.
11. **Product layer**: versioned semantic tokens; fixed component API/DataGrid scope; samples
    checked-in and CI-tested; media/print/export optional.

Deferred (as defined in the ledger): rich text/HTML clipboard, streaming/subscriptions, cross-window overlays, per-tab
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
