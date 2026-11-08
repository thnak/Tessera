# App-completeness gap resolutions v1

Status: accepted (docs-only). Decision: [ADR-033](../../decisions/ADR-033-app-completeness-gap-resolutions.md) (app-completeness gap resolutions). This ledger closes or explicitly defers the
P0/P1 gaps and open questions in
[`app-completeness-cross-check-v1.md`](app-completeness-cross-check-v1.md). The normative behavior
lives in the linked specs; this document records the decision and its status.

## 1. Rules

G-nn identifies a gap and Q-nn an open question; both are defined in the cross-check (G-nn in its section 3.2, Q-nn in its section 4). P0 means the gap must be designed before implementation (cross-check section 3.2).

- **Resolved**: a concrete default/contract is chosen and added to the spec.
- **Deferred**: explicitly out of [V1](../reference/glossary.md), with a reason and a future entry point.
- **Open**: still requires product/platform input; must not block the P0 implementation slice.
- A decision is not an implementation claim; code/tests still need to be built.
- No gap is "closed" by silently deleting the question.

## 2. P0 gap decisions

| ID | Status | Decision | Canonical section |
|---|---|---|---|
| G-01 | Resolved | `TextDocument` uses a contiguous buffer <=64 KiB and a piece table above it; grapheme/paragraph index is incremental and versioned by `revision`. Undo cap: 4 MiB/256 commands per field. | [`text-editing-v1`](../specs/text-editing-v1.md) §2/§10.1 |
| G-02 | Resolved | Selection handles are owned by `ui/editing`, 44 dp touch targets; long-press selects a word; handle drag claims the gesture arena before scroll. Platform magnifier is used if available, framework loupe otherwise. | [`text-editing-v1`](../specs/text-editing-v1.md) §10.1, [`scroll-and-gestures-v1`](../specs/scroll-and-gestures-v1.md) §9.1 |
| G-03 | Resolved | Autofill uses the platform credential/autofill service via the `autofill` hint; no framework password vault. Secure fields never log/persist/clipboard by default. Save/update flows are platform-owned. | [`text-editing-v1`](../specs/text-editing-v1.md) §4/§10.1 |
| G-04 | Resolved | V1 uses the platform spellcheck/autocorrect service only, disabled for secure/code fields; no network spellcheck. Custom dictionaries are deferred. | [`text-editing-v1`](../specs/text-editing-v1.md) §10.1, [`platform-shell-v1`](../specs/platform-shell-v1.md) §9.1 |
| G-05 | Deferred | V1 clipboard is text/plain only; HTML/RTF is sanitized to plain text on paste. Rich-text editing is not planned for V1. | [`text-editing-v1`](../specs/text-editing-v1.md) §6 |
| G-06 | Resolved | Gesture recognizers/arena live in `ui/input`; recognizers claim/reject per pointer. System edge/back/home gestures are HAL (hardware/OS abstraction layer) policy and cannot be swallowed. Custom elements may consume only declared gestures. | [`scroll-and-gestures-v1`](../specs/scroll-and-gestures-v1.md) §9.1 |
| G-07 | Resolved | Scrollbars are overlay by default (8 px visual, 24 px hit); sticky nodes clamp within their scroll container; RTL uses logical axes. | [`scroll-and-gestures-v1`](../specs/scroll-and-gestures-v1.md) §9.1 |
| G-08 | Resolved | One default `OverlayHost` per window; additional hosts are explicit. Layer nodes live under the host (no reparenting), render after base, and modal entries make lower layers inert in input/semantics. Cross-window overlays require a window spec (deferred). | [`overlays-and-layers-v1`](../specs/overlays-and-layers-v1.md) §8.1 |
| G-09 | Resolved | A shared `BackDispatcher` orders: topmost dismissible overlay -> modal route -> route stack -> OS exit. A handler returning handled stops propagation. | [`navigation-v1`](../specs/navigation-v1.md) §10.1, [`overlays-and-layers-v1`](../specs/overlays-and-layers-v1.md) §8.1 |
| G-10 | Resolved | Form schema/type is generated from field declarations; nested paths and keyed arrays are supported; cross-field validators run in topological order and cycles are compile errors. | [`forms-and-validation-v1`](../specs/forms-and-validation-v1.md) §8.1 |
| G-11 | Resolved | Server errors map to `Issue{path,code,severity,message_key}`; reset/submit use epochs; autofill is a value source, and untouched fields may merge while touched fields keep user input + conflict issue. | [`forms-and-validation-v1`](../specs/forms-and-validation-v1.md) §8.1 |
| G-12 | Resolved | Guards return `Allow/Redirect/Deny/Login`, run with an epoch/cancel, 5 s default timeout, max 8 redirects, and one commit at a time. | [`navigation-v1`](../specs/navigation-v1.md) §10.1 |
| G-13 | Resolved | Route loaders start after guards, may mount a skeleton, and store results in `async-data`; prefetch is intent-based. Navigation stores only serializable UI state. | [`navigation-v1`](../specs/navigation-v1.md) §10.1, [`async-data-v1`](../specs/async-data-v1.md) §8.1 |
| G-14 | Resolved | Restore snapshot is canonical JSON v1, <=256 KiB/window, debounced 1 s + forced on background/close; keys are route id + numeric `uiId` + `ItemKey`; migrations are explicit. | [`state-restoration-v1`](../specs/state-restoration-v1.md) §7.1 |
| G-15 | Resolved | Shell capabilities are a bitmask; unsupported/permission-denied returns `Unsupported/Denied`. External file promises cap at 64 MiB/16 files. Component shortcuts are scoped; app/global shortcuts win and conflicts are diagnostics. | [`platform-shell-v1`](../specs/platform-shell-v1.md) §9.1 |
| G-16 | Resolved | Catalogs are JSON + ICU (International Components for Unicode) MessageFormat, compiled to generated C++/binary; ICU data is static/reduced and pinned; locale packs are bundled; no runtime source parsing. | [`i18n-v1`](../specs/i18n-v1.md) §8.1 |
| G-17 | Resolved | Pseudo-locales: `en-XA` for expansion and `ar-XB` for RTL. Directional icons declare `mirror:auto|never`; logos never mirror; missing glyph fallback is a diagnostic. | [`i18n-v1`](../specs/i18n-v1.md) §8.1 |
| G-18 | Resolved | Platform role/action matrix is normative; action requests carry an epoch and run on the UI thread. Announcements dedupe identical messages within 250 ms and progress within 1 s. | [`accessibility-v1`](../specs/accessibility-v1.md) §10.1 |
| G-19 | Resolved | Semantics snapshots are CI (continuous integration) gates; Windows/Linux screen-reader smoke runs nightly. Custom/3D elements require a non-visual fallback + semantics snapshot. | [`accessibility-v1`](../specs/accessibility-v1.md) §10.1 |
| G-20 | Resolved | Data keys use canonical JSON + hash; scope is mandatory. Disk cache is optional and encrypted for sensitive data. Offline queue cap: 100 mutations/1 MiB; default conflict = server-wins + surfaced conflict, merge is opt-in. | [`async-data-v1`](../specs/async-data-v1.md) §8.1 |
| G-21 | Deferred | V1 supports cursor/page pagination; streaming and subscriptions are deferred. | [`async-data-v1`](../specs/async-data-v1.md) §8.1 |
| G-22 | Resolved | Devtools is JSON-lines protocol v1 over stdio/per-user socket with a session token; hot reload uses style patch or structural reconcile, state preserved by component/key; failed compile keeps last good frame; one primary client. | [`devtools-protocol-v1`](../specs/devtools-protocol-v1.md) §7.1 |
| G-23 | Resolved | Full package is V1; delta is deferred. Ed25519 signed manifest, signed key rotation, health check on first frame + 2 failed launches -> rollback; consent before apply; check/download policy explicit. | [`updates-and-distribution-v1`](../specs/updates-and-distribution-v1.md) §6.1 |
| G-24 | Resolved | Crashpad where available, platform handler fallback; internal symbol store; opt-in upload; group by top 3 frames + build id; retention configurable (default 90 days). | [`crash-reporting-v1`](../specs/crash-reporting-v1.md) §6.1 |
| G-25 | Resolved | Token JSON v1 is semantic/versioned; component API contract includes controlled state/focus/a11y; initial DataGrid = virtual rows/columns, selection, sort, resize/reorder, keyboard; editing/grouping/pivot deferred; the library ships after V1. | [`component-library`](../design/component-library.md) §8.1 |
| G-26 | Resolved | Samples are checked-in, generator-compatible, CI-built/tested, with public APIs and reviewed [goldens](../reference/glossary.md); mobile sample deferred. | [`sample-apps`](../guides/sample-apps.md) §4.1 |
| G-27 | Deferred | Media/print/PDF/export remain optional app/platform packages; video uses external texture/custom element; audio focus is platform policy; no core media engine. | [`media-and-export`](../design/media-and-export.md) §6.1 |
| G-28 | Resolved | Animation/interaction is normative: overscroll is a separate visual state applied to the content layer; modes `None/Glow/Stretch/Bounce/Platform`; asymptotic drag resistance + damped spring-back; interruptible; reduced motion clamps; no offset/layout/focus/ID changes. | [`animation-v1`](../specs/animation-v1.md) §7, [`scroll-and-gestures-v1`](../specs/scroll-and-gestures-v1.md) §3.1 |

## 3. Open-question resolutions

| ID | Decision |
|---|---|
| Q-01 | Piece table above 64 KiB; grapheme offsets remain the public edit unit. |
| Q-02 | Text/plain only; rich/HTML deferred. |
| Q-03 | Platform-local spellcheck; no remote provider. |
| Q-04 | Editing owns handles; platform magnifier preferred, framework loupe fallback. |
| Q-05 | HAL reserves system edge/back/home gestures; apps opt in only where the platform allows. |
| Q-06 | Custom elements consume declared gestures via capture/claim; internal scroll must be declared. |
| Q-07 | One overlay host per window; cross-window/nested-host is deferred. |
| Q-08 | BackDispatcher order fixed: overlay -> modal route -> route stack -> OS. |
| Q-09 | Generated form schema from declarations; hand-written validators only for custom rules. |
| Q-10 | Nested paths + keyed arrays; drafts opt-in. |
| Q-11 | One global stack by default; per-tab nested stacks deferred. |
| Q-12 | Route loader runs after guards and stores results in async-data. |
| Q-13 | JSON snapshot v1; encryption for sensitive drafts; 1 s debounce + forced writes. |
| Q-14 | File-promise DnD (drag and drop) resolved; Linux tray is best-effort/deferred. |
| Q-15 | JSON + ICU MessageFormat, compiled; no FTL runtime. |
| Q-16 | Per-window locale override allowed; app-global default. |
| Q-17 | Explicit semantics required for interactive/custom/3D; inference only for text/labels. |
| Q-18 | Semantics snapshot CI + nightly screen-reader matrix. |
| Q-19 | Disk cache optional/encrypted; cross-window invalidation via store events. |
| Q-20 | Default server-wins + conflict error; merge opt-in. |
| Q-21 | Style patch or structural reconcile; state by component/key. |
| Q-22 | Full package v1; delta v1.1; signed key rotation. |
| Q-23 | Crashpad/fallback; internal symbols; opt-in upload. |
| Q-24 | Token schema/API/DataGrid scope fixed as in G-25. |
| Q-25 | Media/export is an optional package, not core. |
| Q-26 | `Platform` resolves overscroll profile via HAL; apps may override per `Scroll`; profiles are typed and calibrated per platform. |

## 4. Deferred with rationale

- Rich-text editing/HTML clipboard: needs a document model, paste sanitization, and editing UX
  beyond V1; plain text is the compatibility baseline.
- Streaming/subscriptions/GraphQL: needs transport/backpressure/consistency design; cursor/page
  pagination covers V1.
- Cross-window overlays and per-tab nested stacks: need multi-window ownership/focus rules.
- Delta updates: full packages are simpler and safer until signing/rollback is proven.
- Linux tray/global shortcuts and mobile media/print: platform support is inconsistent.
- Custom dictionaries, DRM/protected media, PDF/print fidelity: app/platform-specific.
- Scroll-driven/shared-element animations and custom overscroll effects: need broader animation
  model/authoring; core overscroll stretch/spring-back is covered by [ADR-034](../../decisions/ADR-034-interaction-and-overscroll-animation.md) (interaction and overscroll animation).

## 5. Implementation/test follow-up

- Add the decisions in sections 2 and 3 to the specs (done in this change) and keep the gap ledger updated.
- Convert each Resolved decision into an acceptance test in the relevant spec's test section.
- Do not start new areas (rich text/streaming/delta/cross-window) before P0 tests pass.
- After every docs change, manually re-check that links resolve, indexes list every file, Markdown code fences are closed, and ADR counts match (`scripts/` contains only format and lint scripts, so no script performs these checks).
