# App-completeness cross-check and gap register v1

Status: analysis (docs-only). Scope: the app-level specs added by ADR-029..032, cross-checked
against the existing framework docs. This document does **not** replace the specs; it records
ownership, conflicts, missing contracts, and decisions still needed.

Resolution status: G-01..G-27 and Q-01..Q-25 are resolved or explicitly deferred in
[`app-completeness-gap-resolutions-v1.md`](app-completeness-gap-resolutions-v1.md) (ADR-033).
This document remains the analysis/rationale; the ledger is the decision record.

## 1. Method

1. For each new area, find the existing source-of-truth doc(s): component model, IR, input,
   text, layout, scene graph, startup, services, async, resource lifecycle, diagnostics,
   accessibility, security, versioning, project config, mobile integration.
2. Classify each finding:
   - **OK** — new spec is consistent and adds needed detail.
   - **Duplicate** — two docs define the same thing; choose one canonical owner.
   - **Conflict** — behavior/ownership differs; must be resolved before implementation.
   - **Gap** — no contract exists; add to the spec/detail design.
3. Add "Detail design" sections to each spec for the P0 gaps; track the rest here.

## 2. Canonical ownership

| Area | Normative owner | Existing docs it must compose with | Boundary |
|---|---|---|---|
| Numeric IDs/automation | `automation-ids-v1` | IR, scene graph, diagnostics, headless | IDs are identity, not layout/state |
| Text editing | `text-editing-v1` | text, input, accessibility, forms | `ui/editing` owns model/commands; `ui/text` owns shaping/layout; HAL owns IME/candidate window |
| Scroll/gestures | `scroll-and-gestures-v1` | input, layout, virtualization, mobile | `ui/input` owns recognizers/arena; `Scroll` owns offset/physics; virtualization owns realization |
| Overlays/layers | `overlays-and-layers-v1` | component model, scene graph, input, a11y, navigation | `OverlayHost` owns layer stack; no portals; focus/back are overlay concerns |
| Forms/validation | `forms-and-validation-v1` | text editing, async data, i18n, a11y | library/runtime layer, not intrinsics |
| Navigation | `navigation-v1` | deep linking, startup, overlays | `ui/routes.json` is canonical; deep links map patterns -> route ids |
| State restoration | `state-restoration-v1` | navigation, startup, services, mobile | `services/restore` owns snapshot/write/restore; navigation owns stack semantics |
| Platform shell | `platform-shell-v1` | services, HAL, security, overlays | `services/shell` owns OS capabilities; no OS handles in UI |
| i18n | `i18n-v1` | text, services, assets, dependencies | ICU data/catalogs; text stack consumes locale/direction |
| Accessibility | `accessibility-v1` | design/accessibility, bridges, input, overlays | semantics tree is UI-thread-owned; bridges read snapshots |
| Async data | `async-data-v1` | async runtime, resource lifecycle, services | `data` owns typed domain data/cache; `resources` owns asset bytes; `async` owns tasks |
| Devtools | `devtools-protocol-v1` | diagnostics, designer, headless | runtime inspector, local-only; designer is design-time |
| Updates | `updates-and-distribution-v1` | build/packaging, security, versioning | app updates, not framework install |
| Crash reporting | `crash-reporting-v1` | diagnostics-config, security | capture config in diagnostics; pipeline/symbols/upload here |
| Component library | `component-library` | style/text/accessibility/forms/overlays | optional product layer; no core dependency |
| Samples/media/export | samples/media docs | project generator, renderer, shell | examples/optional integrations, not core |

## 3. Cross-check findings

### 3.1 Duplicates and conflicts to resolve

1. **Route declarations are duplicated.** `deepLinks.routes` in `tessera.json` and
   `navigation.routesFile` both define route patterns/ids. Canonical rule: `ui/routes.json`
   defines screens/params/guards; `deepLinks.routes` only maps an external pattern to an
   existing route id. No screen/component declarations in `deepLinks`.
2. **Three restore flags exist.** `loading.restoreRoute`, `navigation.restore`, and
   `restore.enabled` overlap. Canonical precedence:
   `restore.enabled` = master; `navigation.restore` = route-stack restore; `loading.restoreRoute`
   = auth/deep-link continuation only. A false master disables both.
3. **Two cache systems.** `resource-lifecycle-v1` has `ResourceClass::Data` and disk cache;
   `async-data-v1` also defines cache/offline. Canonical rule: resource lifecycle owns
   bytes/assets (fonts, images, shaders, media); async data owns typed domain records/queries.
   Network transport is `services/networking`; async data does not load asset bytes.
4. **Three debug/inspection surfaces.** `diagnostics.overlay`, `devtools-protocol-v1` inspector,
   and `designer` preview are different. Canonical rule: diagnostics overlay = performance
   metrics; devtools inspector = runtime tree/style/semantics/hot reload; designer = Wasm
   design-time preview. They share snapshot schemas but not lifecycle/security.
5. **Crash config vs crash pipeline.** `diagnostics-config-v1` owns capture toggles/dir/
   breadcrumbs; `crash-reporting-v1` owns dump format, symbols, consent, upload, grouping.
   Neither should add a second `crashDumps` switch.
6. **Overlay wording conflict.** `component-model-v1`, `custom-elements-v1`, and the custom
   elements guide say "wrap in `View` for overlays"; this contradicts portals being replaced by
   the overlay service. Fix to "use `OverlayHost`/overlay service; `CustomElement` remains a
   leaf."
7. **IME candidate-window ownership.** `input.md` says the platform backend positions the
   candidate window; `text-editing-v1` says HAL normalizes candidate-window rects. Canonical
   rule: HAL/platform positions the native candidate window; UI reports the caret rect; text
   editing owns composition state, not native window placement.
8. **Accessibility role/action sets differ.** `design/accessibility.md` lists a smaller V1 role
   set than `accessibility-v1`. The normative spec wins; the design doc is rationale/summary.
9. **Versioning omits new artifacts.** `versioning-v1` does not list restore snapshots, update
   manifests, i18n catalogs, devtools protocol, crash reports, or design-token packs.
10. **Services implementation order is stale.** `design/services.md` still ends at
    storage/i18n/navigation/networking/notifications; add shell/restore/data and align with
    ADR-030.
11. **`CustomElement` overlay guidance is repeated in three places** and one open question
    ("2D overlays inside inline") needs to defer to the overlay spec.
12. **`loading.restoreRoute` is ambiguous** (last route vs deep link vs auth continuation).
    Document it as "resume the pending activation/route after auth" and point stack restore to
    `navigation.restore`/`restore.enabled`.

### 3.2 P0 gaps (must be designed before implementation)

| ID | Area | Gap | Impact |
|---|---|---|---|
| G-01 | Text | Large-document model (rope/piece table) and stable grapheme/paragraph indices | Undo/selection/IME degrade or allocate per edit |
| G-02 | Text | Touch selection handles/magnifier, long-press, drag-select vs scroll/gesture arena | Mobile editing conflicts with scrolling |
| G-03 | Text | Autofill/credential/password-manager integration and secure-input sessions | Password UX/security incomplete |
| G-04 | Text | Spellcheck/autocorrect/text replacement and privacy policy | Platform parity gap |
| G-05 | Text | Rich/HTML clipboard and paste sanitization | Interop with other apps |
| G-06 | Scroll | Gesture arena API: claim/reject/simultaneous, custom element consumption, system edge/back gestures | Drag/scroll/custom input ambiguities |
| G-07 | Scroll | Scrollbar interaction, sticky/anchoring algorithm details, RTL axes | Inconsistent desktop/mobile behavior |
| G-08 | Overlay | `OverlayHost` scene/dirty/semantics integration, multiple hosts, cross-window | Layer stack cannot be implemented safely |
| G-09 | Overlay | Back/Escape ordering across overlay -> modal route -> route stack -> OS exit | Double-dismiss or lost back |
| G-10 | Forms | Field schema/type generation, arrays/nested fields, cross-field cycles | Large forms unmanageable |
| G-11 | Forms | Server error mapping, reset/submit races, autofill merge policy | Data loss/incorrect errors |
| G-12 | Navigation | Guard API/epoch/timeout, redirect-loop detection, transition reentrancy | Race/crash on rapid navigation |
| G-13 | Navigation | Route-level data loading/prefetch contract | Loading/error/empty states ad-hoc |
| G-14 | Restore | Snapshot schema generation/migration, stable keys for virtualized items, write cadence | Corrupt/lost/expensive restore |
| G-15 | Shell | Capability negotiation/fallback, file-promise DnD, menu/shortcut conflict resolution | Silent no-ops/platform divergence |
| G-16 | i18n | Catalog format/build pipeline (ICU MessageFormat vs FTL), data packaging, input parsing | Tooling undefined |
| G-17 | i18n | RTL icon mirroring policy, font fallback per locale, pseudo-locale coverage | Visual/RTL bugs |
| G-18 | A11y | Platform role/action mapping, action callback threading, announcement dedupe | Screen-reader gaps |
| G-19 | A11y | Screen-reader automation and 3D/custom fallback testing | Cannot gate CI |
| G-20 | Data | Cache-key serialization/scope, disk cache/encryption, offline mutation conflict | Wrong-user data/duplication |
| G-21 | Data | Streaming/subscriptions and pagination/scroll coupling | Large feeds incomplete |
| G-22 | Devtools | Protocol schema/version, session auth, hot-reload patch format/state preservation | Unsafe/brittle tooling |
| G-23 | Updates | Delta tooling, key rotation, health-check/rollback policy, consent | Supply-chain/recovery gaps |
| G-24 | Crash | Dump handler per OS, symbol server/retention, consent UX, grouping | Unactionable reports |
| G-25 | Library | Token schema/versioning, component API contract, DataGrid scope | Visual/API churn |
| G-26 | Samples | CI/maintenance/golden ownership; mobile sample | Docs rot |
| G-27 | Media | Media service vs custom element, audio focus, printing/PDF determinism | Scope creep/duplicate integrations |

### 3.3 P1 gaps (after P0)

- Text search/replace, multi-caret, IME candidate styling, accessibility text navigation by
  word/line.
- Scroll parallax/pull-to-refresh, overscroll effects, paging indicators.
- Overlay sheet/drag-to-dismiss, nested menus, toast queue policy.
- Forms wizard/multi-step, file fields, dynamic validation schemas.
- Navigation browser-history integration, nested stacks per tab, route transitions.
- Restore cross-device/cloud, encrypted drafts, multi-window conflict merge.
- Shell global shortcuts, tray on Linux, notifications integration.
- i18n collation/search, locale-aware sorting, translation workflow.
- A11y switch control/voice control, high-contrast token audit.
- Data GraphQL/streaming/subscriptions, offline conflict UI.
- Devtools time-travel, IDE integration, multi-client.
- Updates enterprise/MDM, portable install, LTS.
- Crash GPU device-loss reports, breadcrumb schema.
- Library charts/date picker/data grid editing, icon set.
- Samples localization/accessibility variants, mobile.

## 4. Consolidated open questions

| ID | Area | Question | Needed decision |
|---|---|---|---|
| Q-01 | Text | UTF-8 grapheme offsets vs opaque handles for large docs | Data model/perf budget |
| Q-02 | Text | Rich/HTML clipboard and paste sanitization | Security/interop scope |
| Q-03 | Text | Spellcheck/autocorrect provider and privacy | Platform/service boundary |
| Q-04 | Text | Touch selection handles/magnifier ownership | Input vs editing vs HAL |
| Q-05 | Scroll | System gesture precedence (back/edge/home) vs app scroll | HAL policy |
| Q-06 | Scroll | Custom element internal scroll/drag consumption | Element API |
| Q-07 | Overlay | Multiple overlay hosts/cross-window/route-owned overlays | Layer ownership |
| Q-08 | Overlay | Back ordering: overlay vs modal route vs route stack | Navigation contract |
| Q-09 | Forms | Schema/type generation vs hand-written validators | Compiler/tooling |
| Q-10 | Forms | Field arrays/nested fields identity and draft persistence | Data model |
| Q-11 | Nav | Nested stacks per tab/modal vs one global stack | Stack model |
| Q-12 | Nav | Route data loading/caching contract | Data/navigation boundary |
| Q-13 | Restore | Snapshot format (JSON vs binary), encryption, write cadence | Storage/perf |
| Q-14 | Shell | File-promise DnD/cross-process and Linux tray support | Platform scope |
| Q-15 | i18n | Catalog format and runtime language packs | Tooling |
| Q-16 | i18n | Per-window locale vs app-global | State model |
| Q-17 | A11y | Inferred vs explicit semantics; 3D/chart fallback | Authoring contract |
| Q-18 | A11y | Screen-reader automation matrix/CI | Test infrastructure |
| Q-19 | Data | Disk cache/encryption and cross-window invalidation | Storage/security |
| Q-20 | Data | Offline mutation conflict policy | UX/data model |
| Q-21 | Devtools | Hot-reload patch format and state preservation | Compiler/runtime |
| Q-22 | Updates | Delta format/key rotation/rollback health | Release engineering |
| Q-23 | Crash | Handler/symbol server/consent/retention | Platform/privacy |
| Q-24 | Library | Tokens versioning, headless vs styled split, DataGrid scope | Product layer |
| Q-25 | Media | Default media service vs app/platform package | Scope |

## 5. Implementation order (dependency-aware)

1. **Contracts/config**: route table canonicalization, restore precedence, cache boundary,
   overlay wording, versioning table.
2. **Input primitives**: gesture arena + focus/inert + IME composition + text commands.
3. **Text editing + forms**: document model, selection/IME/undo, field/validation.
4. **Scroll + virtualization + overlays**: physics/nested scroll, overlay host/focus/back.
5. **Navigation + restore + shell**: stack/guards/deep links, snapshot/migration, clipboard/DnD/
   dialogs.
6. **i18n + accessibility**: catalogs/RTL, semantics/actions/announcements.
7. **Async data/offline**: cache/query/mutation/offline queue.
8. **Devtools + updates + crash**: local inspector/hot reload, signed updates, reports.
9. **Component library + samples + media**: product layer/examples/optional integrations.

## 6. Detail design coverage

| Area | Detail-design section |
|---|---|
| Text editing | `text-editing-v1 §10.1` (ownership, document/controller/layout, command/IME/undo invariants) |
| Scroll/gestures | `scroll-and-gestures-v1 §9.1` (arena, precedence, state machine, nested/anchor) |
| Overlays | `overlays-and-layers-v1 §8.1` (host/entry state, layering, back ordering) |
| Forms | `forms-and-validation-v1 §8.1` (field registry, validation pipeline, submit/races) |
| Navigation | `navigation-v1 §10.1` (entry/epoch/guard/back/route-data contract) |
| Restore | `state-restoration-v1 §7.1` (snapshot schema, write cadence, restore order) |
| Platform shell | `platform-shell-v1 §9.1` (capabilities, shell task contract, DnD state) |
| i18n | `i18n-v1 §8.1` (catalog pipeline, resolution, caches, RTL) |
| Accessibility | `accessibility-v1 §10.1` (semantics diff, actions, focus, announcements, bridges) |
| Async data | `async-data-v1 §8.1` (keys/scope, query/mutation states, offline) |
| Devtools | `devtools-protocol-v1 §7.1` (protocol/session, hot reload, security) |
| Updates | `updates-and-distribution-v1 §6.1` (verify/stage/health/rollback) |
| Crash | `crash-reporting-v1 §6.1` (capture/symbols/consent/upload) |
| Library | `component-library §8.1` (tokens, API, DataGrid, tests) |
| Samples | `sample-apps §4.1` (CI, template, maintenance) |
| Media/export | `media-and-export §6.1` (playback/print/export boundaries) |

The detail sections define ownership, invariants, ordering, and failure behavior. The remaining
G/Q items are deliberately still open (tooling/format/platform choices), not silently resolved.

## 7. Acceptance checklist

- [ ] Every area has one canonical owner and no duplicate route/cache/restore declarations.
- [ ] Detail-design sections exist for G-01..G-27 (or explicitly deferred).
- [ ] `versioning-v1` lists every new artifact/version.
- [ ] Overlay/portal wording is consistent in component model, custom elements, guide.
- [ ] IME candidate-window ownership is consistent.
- [ ] Restore precedence is documented in config/navigation/startup.
- [ ] Async data vs resource lifecycle boundary is documented in both specs.
- [ ] Diagnostics/devtools/designer surfaces are distinct.
- [ ] Open questions have owners/decisions or are marked deferred.
- [ ] Links, indexes, code fences, and ADR counts remain valid.
