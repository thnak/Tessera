# Services: i18n, navigation, storage, networking, notifications

Status: design for [Big Step F](../../../AGENTS.md#big-steps-group-work-dont-micro-step) (production breadth). `src/services/*` are placeholders.

Services are app-facing capabilities that are not part of the renderer. They follow the same
runtime rules: async, cancellable, bounded, no UI-thread blocking, and no hidden global state.

Normative companions: [`navigation-v1.md`](../../specs/navigation-v1.md),
[`i18n-v1.md`](../../specs/i18n-v1.md), [`platform-shell-v1.md`](../../specs/platform-shell-v1.md),
[`async-data-v1.md`](../../specs/async-data-v1.md), and
[`state-restoration-v1.md`](../../specs/state-restoration-v1.md).

This folder is the design set for those services. The former single-file `docs/design/services.md`
reached the 600-line hard limit, so it was split here. Section numbers are unchanged, so a reference
that names a section still names the same heading; a reference that used to address a heading anchor
inside that file now has to name the part that holds the heading (for example section 11.1, formerly
`#111-who-marks-the-in-frame-lookup-region-the-appfw-tick-wrapper`, is in
[`app-lifecycle-rulings.md`](app-lifecycle-rulings.md)). Start from the part below that owns the
module you need.

## Parts

| Part | Owns |
|---|---|
| [`common-contract.md`](common-contract.md) | Section 1: the shared `Service`/`Result` contract, the async rules every service follows, the AF1 shape that landed (ADR-050 section 4) |
| [`i18n.md`](i18n.md) | Section 2: ICU, catalogs, locale negotiation, and what a locale change dirties (AF8) |
| [`navigation.md`](navigation.md) | Section 3: routes, stack and back priority, deep links, restore snapshots (issue #196) |
| [`storage.md`](storage.md) | Section 4: settings/documents/cache classes, atomic replace, durability, corruption fallback (AF7) |
| [`platform-modules.md`](platform-modules.md) | Sections 5, 6 and 8: networking, notifications, security and privacy rules (targets only) |
| [`background-and-lifecycle.md`](background-and-lifecycle.md) | Section 7 and 11.4: suspend/resume expectations, `TaskScope`, the layer-0 timer wake source (AF2) |
| [`app-lifecycle-rulings.md`](app-lifecycle-rulings.md) | Sections 11-11.3: `Application`, ordered dispatch, the in-frame lookup mark, the start deadline (AF3) |
| [`service-injection.md`](service-injection.md) | Sections 11.5-11.6: `useService<T>()` in IR and codegen, and what the TSX half still needs (AF5) |
| [`status-and-open-questions.md`](status-and-open-questions.md) | Sections 9 and 10: implementation order and what is still open |

Related: [`../async.md`](../async.md) (executors and the completion queue),
[`../background-work.md`](../background-work.md) (`TaskScope` in full),
[`../storage-module.md`](../storage-module.md) (the longer storage rationale),
[`../observable.md`](../observable.md), [`../service-resolve-benchmark.md`](../service-resolve-benchmark.md),
[`../security.md`](../security.md), and
[ADR-050](../../../decisions/ADR-050/README.md).
