# App framework: proposed answers to the ADR-050 open questions

Status: all nine answers accepted by the owner on 2026-11-12 (the text below keeps the original
reasoning). Recommendations for the nine open questions of
[ADR-050](../../decisions/ADR-050-opt-in-app-framework-layer.md) ("Questions for the owner",
"Still open"), written for the owner to accept or change. Nothing here is decided until the owner
says so; ADR-050 stays Proposed. Each answer gives the options, a recommendation, the trade-off
accepted, the seam to leave now, the risk and a trigger for revisiting. Section 10 lists the
contradictions with existing documents that the recommendations would require fixing.

Inputs read: ADR-050 and the documents it links
([`app-framework-gap-audit-v1.md`](../architecture/app-framework-gap-audit-v1.md),
[`native-render-surfaces.md`](native-render-surfaces.md),
[`service-resolve-benchmark.md`](service-resolve-benchmark.md)),
[`startup-api-v1.md`](../specs/startup-api-v1.md), [`services.md`](services.md),
[`app-host.md`](app-host.md), [`dirty-rendering.md`](dirty-rendering.md),
[`devtools-protocol-v1.md`](../specs/devtools-protocol-v1.md),
[`navigation-v1.md`](../specs/navigation-v1.md),
[`platform-shell-v1.md`](../specs/platform-shell-v1.md),
[`os-events-v1.md`](../specs/os-events-v1.md), [`dependencies.md`](../specs/dependencies.md),
[`gpu/rhi.md`](gpu/rhi.md), ADR-040, ADR-049 (branch `docs/adr-049-backend-extensibility`,
[PR #83](https://github.com/thnak/Tessera/pull/83)), and the code seams
`include/tessera/app/window.hpp`, `src/app/window_impl.hpp`, `src/app/window.cpp`,
`include/tessera/ui/scene/update_scheduler.hpp`, `include/tessera/runtime/*`, `src/services/*`
(all `.gitkeep`).

Comparisons with Qt, JUCE, Flutter, Compose and React Native are used only where they sharpen an
argument. Statements marked **(unverified)** are from memory of those projects and should be
checked before they are quoted as fact.

## Summary

| # | Question | Recommendation in one line |
|---|---|---|
| 1 | OS-scheduled background work, tray apps | Not in V1 for WorkManager/BGTaskScheduler; tray-resident desktop apps are V1 (window-less `Hidden` state); leave a window-less `Application` start and a `BackgroundTask` interface as seams |
| 2 | Route-scoped services | App and window scopes only; per-route state lives in `NavEntry.state`, component state and the `Route` resource scope; make scopes a parent chain so a third level is additive |
| 3 | One `Application`, several windows | Yes in V1 on desktop, designed N-window from AF3 but shipped as its own slice after the single-loop/shared-device refactor; mobile stays one window |
| 4 | SQLite | In-tree, opt-in module (`tessera_appfw_storage_sqlite`), vendored amalgamation, not default-linked, runs only on the I/O executor; ORMs, other engines and sync stay community |
| 5 | HTTP stack | Platform stacks in-tree (WinHTTP, NSURLSession, browser `fetch`, Android later), libcurl only as the Linux back end; one contract suite pins the behavior |
| 6 | Action log; observables in TSX render | Plain observables plus a debug-only change trace, no action log; allow reads in render only through an explicit `useObservable(svc.member)` lowered to a mount-time subscription |
| 7 | Startup spec revision | `Application` owns P1-P5 and launch-mode detection, `AppHost` stays the per-window frame loop; the C++ lifecycle API is V1; JSX reads lifecycle through a built-in `AppLifecycle` service, not new hooks |
| 8 | Default module set | `storage`, `navigation`, `restore`, `i18n`; move `data` to opt-in; admit a module to the default set only by stated criteria and a measured size delta |
| 9 | Worker-rendered and OS sub-surfaces | Neither in V1; constrain `on_render` now so it can move to a worker without an API break; OS sub-surfaces are a per-backend ADR-049 extension triggered by protected video or external engines |

## 1. OS-scheduled background work and tray-resident desktop apps

**Options.**

| Option | For | Against |
|---|---|---|
| A. V1: headless entry with WorkManager/JobScheduler and BGTaskScheduler adapters | complete mobile story from day one | the Android and iOS platform back ends do not exist (`src/platform/android/` is empty; iOS is "later" in Big Step F); nothing to test against |
| B. Later, but fix the contract shape now | no dead code; the seam keeps the headless entry possible | apps needing periodic sync wait for Big Step F |
| C. Never in-tree; community plugin | smallest core | the headless entry needs `Application` to start without a window, which only core can guarantee |

**Recommendation: B for OS schedulers; tray-resident desktop apps are in V1.**

- OS-scheduled work belongs to the mobile platform back ends, which are Big Step F work. Building
  the adapter before an Android lifecycle exists means designing against documentation only.
- The tray case is already V1 elsewhere: [`platform-shell-v1.md`](../specs/platform-shell-v1.md)
  section 1 marks "Tray/status item" as V1 on Windows and macOS. ADR-050 only needs to make the
  lifecycle consistent with it: `quitOnLastWindowClosed = false` (default `true`) keeps the
  `Application` in `Hidden` with zero windows, services keep running under their
  `BackgroundPolicy`, and the tray item (owned by the `shell` module) reopens a window. This is the
  same shape as Qt's `QGuiApplication::quitOnLastWindowClosed` property.
- The policy that `platform-shell-v1.md` section 6 leaves open ("battery/visibility policy") becomes
  the `Hidden` row of ADR-050 section 5: no rendering, no GPU work, timers still join the pacer
  deadline so a tray app with no due timer parks.

**Trade-off accepted.** Mobile apps cannot do periodic background sync in V1. Desktop apps can run
work while hidden, which is where V1 apps live first (Windows-first lane).

**Seams to leave now.**

1. `Application` must be startable and testable with **zero windows** (the AF3 headless tests do
   this), and P1 must not assume that it creates a window. The headless OS entry is then "the same
   `Application`, no window, a declared subset of services".
2. A `BackgroundTask` interface in `tessera_appfw_core`, unimplemented by any scheduler:
   `static constexpr TaskKindId kTaskKind`, `runtime::Task<BackgroundResult> run(BackgroundContext&)`
   where the context carries a deadline, a cancellation token and the services the task declared.
   Desktop code can already run it inside a `TaskScope`; a mobile scheduler adapter later only
   decides *when* to call it.
3. Services declare whether they are available in a headless start (`kHeadlessCapable`, default
   false); startup validation can then reject a background task that requires a service that never
   starts headless.
4. Reserve an activation source `background_task` next to `deep_link` and `notification` in
   [`startup-api-v1.md`](../specs/startup-api-v1.md) section 5.1 (reserved, not V1).

**Risk.** iOS requires BGTaskScheduler launch handlers to be registered before the app finishes
launching **(unverified detail; check Apple's BGTaskScheduler documentation)**, and Android
WorkManager can start the process with no activity. Both mean the headless entry must run before
or without P1's window. Seam 1 is what protects against this; if AF3 couples `Application` to a
window, the risk becomes a rewrite.

**Revisit when** the first mobile platform back end (Android) reaches lifecycle mapping in Big Step
F, or an app needs periodic sync on mobile.

## 2. Route-scoped services

**Options.** (A) App and window scopes only; (B) a third scope per navigation entry, created on push
and destroyed on pop (similar to a `ViewModel` scoped to a `NavBackStackEntry` in Navigation-Compose,
or route-level providers in Angular); (C) arbitrary nested scopes under any component.

**Recommendation: A in V1.**

- [`navigation-v1.md`](../specs/navigation-v1.md) section 10.1 already gives each `NavEntry` a
  serializable `state` map and a stable `entry_id`. Per-screen state that must survive while a
  screen is off-screen fits there and is restored with the stack; per-screen work belongs to the
  screen component's `TaskScope` (cancelled on unmount); per-route assets use the `Route` resource
  scope of [`startup-api-v1.md`](../specs/startup-api-v1.md) section 4.3.
- A route scope needs the navigation runtime (not implemented), a decision about whether off-stack
  screens stay mounted (navigation-v1 does not say), and a third resolution level whose lifetime is
  shorter than the components that may have cached a pointer to it. With resolve-at-mount, a
  component mounted under one route must never outlive that route's scope; that is true only if
  every screen is unmounted before its entry is popped, which is again an unstated navigation rule.
- C reintroduces React context, which the component model rejects.

**Trade-off accepted.** An app that wants a per-screen "view model" object writes it as state of
the screen's developer class (ADR-040) or keys it by `entry_id` inside an app-scoped service.

**Seams to leave now.**

1. Implement `ServiceContext` scopes as a **parent chain** (`window -> app`), not as two hard-coded
   levels, so a `route -> window -> app` chain is additive.
2. The generated requirement manifest (ADR-050 section 3 item 6) records the scope at which each
   requirement was satisfied, so startup validation can later check "this component is mounted only
   under routes that provide X".
3. Keep `NavEntry.entry_id` stable and exposed to C++; a route scope would key on it.

**Risk.** Apps put per-screen state in app-scoped services and leak it across screens (stale draft
in a reused editor). Mitigation: guidance in the future `app-framework.md` design note.

**Revisit when** navigation-v1 is implemented and decides that popped or off-stack screens keep
state without staying mounted, or when two sample apps hand-roll the same per-entry holder.

## 3. One `Application` with several windows in V1

**What the code says.** `WindowHost` is "one window per host", owns its own `rhi::Device`
(`src/app/window.cpp`, `device = detail::create_device()`) and its own `RuntimeLoop`
(`src/app/window_impl.hpp`), and `run()` blocks until the window closes. Two `WindowHost`s on one
thread therefore cannot run at the same time, and two devices would not share pipelines, glyph
atlases or uploaded images.

**Options.** (A) One window per process in V1; (B) N windows under one `Application` in V1;
(C) N-window API now, single window implemented first, N windows as a later V1 slice.

**Recommendation: C, with N windows shipping in V1 on desktop; one window on mobile.**

- ADR-050 already assumes several windows: the app-state table derives `Active` from "a window
  focused" and `Inactive` from "all windows blurred", window scopes exist, and
  [`navigation-v1.md`](../specs/navigation-v1.md) gives each window its own stack.
  [`deep-linking-v1.md`](../specs/deep-linking-v1.md) section 6 has `openIn=newWindow`. Every
  comparable desktop framework (Qt, JUCE) supports it; Flutter's desktop multi-window support
  arrived late and is still limited **(unverified current status)**, which is the cost of not
  designing for it early.
- Getting there needs a refactor that is cheap now and expensive later: one `RuntimeLoop`, one
  clock, one completion queue and one `rhi::Device` per `Application`; each window keeps a
  swapchain, an `AppHost`, an `UpdateScheduler` and a frame pacer; `WindowHost::run()` becomes a
  non-blocking per-tick `pump()` driven by the `Application` loop, and the existing blocking `run()`
  stays as a thin single-window wrapper for samples and tests.
- Startup is unchanged: the `<App>` subtree describes the **main** window. Secondary windows are
  opened at run time from C++ (`app.open_window(factory, options)`), have no loading phase, and are
  not startup declarations, which keeps [`startup-api-v1.md`](../specs/startup-api-v1.md) section
  10 ("multiple windows started in one App subtree") deferred as written.

**Trade-off accepted.** One device for all windows means a device loss affects every window, and
`DeviceRecoveryController` moves from per-window to per-device.

**Seams to leave now.** AF3 builds `Application` with a window list and tests two headless windows
(scripted focus/blur proving the `Active`/`Inactive` aggregation); `ServiceContext` window scopes
are per window from the start; new code never reaches for "the" window.

**Risk.** Per-window pacing on monitors with different refresh rates
([`runtime-config-v1.md`](../specs/runtime-config-v1.md) lists "multi-window refresh/VRR
independence" as open) with one loop: the loop must wake on the earliest of the windows' pacer
deadlines. This is the main design work of the slice.

**Revisit when** the single-loop refactor is estimated: if it exceeds one Big Step slice, ship V1
single-window and keep the API N-window.

## 4. SQLite adapter: in-tree or community only

**Facts.** SQLite is public domain (no attribution required; [`dependencies.md`](../specs/dependencies.md)
section 7 already lists `sqlite3` as "V1 optional" for storage), ships as a single-file C
amalgamation that links statically on every target, and its API is blocking.

**Options.** (A) In-tree module, opt-in; (B) in-tree and default-linked; (C) community only;
(D) use the OS copy (Android and iOS ship one).

**Recommendation: A.** A module `tessera_appfw_storage_sqlite` with a vendored, pinned amalgamation,
enabled per app in `tessera.json`, not part of the default template set.

- Licensing is the easiest possible case; static linking is natural; the dependency rules of
  ADR-049 section 5 are met with one row in `dependencies.md` and `THIRD_PARTY_NOTICES.md`.
- Most real apps need structured local data; if core does not ship one adapter, every community
  adapter invents its own threading model, and the most common mistake (calling SQLite on the UI
  thread) becomes the default. Owning the adapter lets core enforce the rule: all calls run on the
  `IoExecutor` (one connection per strand), results return through the `CompletionQueue`, and
  cancellation uses `sqlite3_interrupt`.
- The surface stays thin: open, prepared statements with typed binds, transactions, migrations by
  `user_version`. No ORM, no query builder.
- Contract tests need no fake: SQLite's in-memory database (`:memory:`) is deterministic and fast,
  so the contract suite runs against the real engine; the in-memory file-system fake from AF7 covers
  the file-level failure cases (corruption fallback, full disk).
- D is rejected: version skew across OS releases breaks determinism and Wasm has no OS copy.
- B is rejected: SQLite adds roughly half a megabyte or more of code **(estimate; measure in the
  slice)** that apps without structured data never use, and section garbage collection cannot drop
  an engine that the module initializes.

**Community scope.** ORMs, other engines (LMDB, DuckDB, ...), encryption (SQLCipher or similar,
license to be checked per product), cloud sync.

**Seam to leave now.** The `storage` module's key/value and document stores do not depend on
SQLite; the SQLite module depends on `storage` (paths, migrations), never the reverse.

**Risk.** Wasm persistence: an Emscripten build works in memory, but durable storage in the browser
needs a VFS over OPFS or IndexedDB with its own threading constraints **(unverified details)**. V1
can accept "in-memory or export/import only" on Wasm, since Wasm is the design-time target.

**Revisit when** the slice measures the size delta, or a second engine is requested often enough
to justify a generic SQL interface.

## 5. Networking: platform HTTP stacks or one vendored stack

**Options.**

| Option | For | Against |
|---|---|---|
| A. Platform stacks: WinHTTP (Windows), NSURLSession (Apple), browser `fetch` (Wasm), Android through JNI (HttpURLConnection or Cronet) | OS trust store, proxy/PAC, enterprise policy, OS security patches, HTTP/2 (and HTTP/3 where the OS offers it), iOS App Transport Security applied, near-zero binary cost | several implementations; behavior differs (redirects, cookies, caching, timeouts) and must be pinned by tests; Linux has no system HTTP stack |
| B. One vendored stack (libcurl plus mbedTLS or OpenSSL) | one behavior everywhere | Tessera owns TLS CVE response for every shipped app; must load each OS trust store and proxy settings itself; adds binary size; still unusable in Wasm, where only `fetch` (and WebSocket) exists |
| C. Interface only in-tree, every back end community | smallest core | an app framework whose template cannot make an HTTPS request is not credible |

**Recommendation: A, with libcurl as the Linux back end only.**

- TLS is the part a framework should least want to own. Platform stacks receive OS security
  updates without the app being rebuilt; a statically linked TLS library does not. This also
  matters for the static-linking rule: every app built with B carries its own TLS copy.
- Wasm forces an asynchronous, request/response-shaped interface anyway (no raw sockets, CORS
  applies), so A costs nothing in API design: the `net` interface is shaped like `fetch`
  (request, streamed or buffered body, cancellation, timeout), and every back end fits it.
- Linux uses libcurl with the distribution's CA bundle; whether TLS comes from a statically linked
  mbedTLS or OpenSSL is decided in the slice (both Apache-2.0; the `dependencies.md` rows exist).
- Windows-first ordering: the fake, then WinHTTP, then Linux libcurl, then `fetch`, then Apple,
  then Android with the mobile back end.
- WinHTTP and NSURLSession deliver callbacks on their own threads; back ends must marshal every
  result through the `CompletionQueue` and never call app code from those threads.

**Trade-off accepted.** Behavior differs at the edges between platforms. The `net` contract suite
fixes the normative behavior (redirect limit, which status codes are errors, timeout semantics,
cancellation, no implicit cookie jar or HTTP cache unless enabled) and each back end configures the
platform stack to match. The suite runs against the deterministic fake in normal CI; real back ends
run it against an in-process loopback server in an opt-in lane (no external network).

**Seams to leave now.** The `net` interface takes no platform types and exposes no socket-level
API; certificate pinning, if ever added, is an interface-level option that each back end implements.
WebSocket is a separate interface, not V1.

**Risk.** HTTP/3 and proxy behavior on Linux (libcurl) will lag the platforms. Some community users
will want one stack everywhere for identical behavior; they can provide a libcurl back end on all
platforms as a Tier 2 provider.

**Revisit when** the contract suite shows a platform difference that configuration cannot remove,
or a platform deprecates its stack.

## 6. State model: plain observables or an action log; observables in TSX render

### 6.1 Action log and devtools time-travel

**Recommendation: plain `Observable<T>` (ADR-050 section 6), no action log in V1.**

- [`devtools-protocol-v1.md`](../specs/devtools-protocol-v1.md) section 8 already defers
  "time-travel/recording format". Its V1 goals are inspection plus **record/replay of input**.
  Tessera's determinism story (fixed-step loop, `ManualClock`, `ManualExecutor`, scripted events)
  gives replay by **re-execution**: replaying the recorded input and completions against fakes
  reproduces state without requiring every state change to be an action.
- An action log forces reducer discipline on all app state, which the V1 surface rejects
  (`useReducer` is unsafe in [`react-compat.md`](../specs/jsx-intrinsics-v1/react-compat.md)) and
  which JUCE's `ValueTree`, Compose's `StateFlow` and Flutter's `ChangeNotifier` also do not
  require. Redux-style time travel in React apps is a library choice, not a framework feature.

**Seam to leave now.** Each observable gets a stable debug name (service id plus member name from
`ui/services.json`) and its version counter. In builds with devtools enabled, `set()` emits a
bounded trace event (name, version, frame index) into the diagnostics ring; release builds compile
this out. A devtools `observables` read command can list current values for types that provide an
optional serialization trait. An app that wants an action log builds a store on top of observables.

**Revisit when** devtools-protocol schedules time-travel, or restore needs service state snapshots.

### 6.2 May TSX read service observables in render?

**Options.** (a) No: C++ mirrors observables into state cells (ADR-050 today); (b) implicit: any
read of a manifest-listed observable in the render body is found by the compiler and subscribed at
mount; (c) explicit hook: `const route = useObservable(nav.route)`, lowered to a mount-time
subscription.

**Recommendation: (c).**

```tsx
export function Header() {
  const nav = useService<Navigator>();
  const route = useObservable(nav.route);     // nav.route: Observable<Route> listed in services.json
  return <Text>{route.title}</Text>;
}
```

- Lowering: `useObservable(x)` requires `x` to be a manifest-listed `Observable<T>` member of a
  `useService` result. Codegen adds a `Subscription` member that subscribes the component's
  `UpdateTicket` in `mount()` and unsubscribes in `unmount()`, and a cached `const Observable<T>*`.
  Render reads `ptr->get()` during the generated `update()`.
- Dirty-rendering fit: `set()` calls `UpdateTicket::schedule()`, which marks the component in its
  window's `UpdateScheduler`; many `set()` calls in one tick cause one `update()` at the next
  `flush()` (`update_scheduler.hpp`), and `update()` raises only the style/layout/paint domains
  that its diffs touch ([`dirty-rendering.md`](dirty-rendering.md) section 2). Nothing re-renders
  synchronously; a static app stays idle because an unchanged value (`operator==`) schedules
  nothing.
- Consistency: all writes happen on the UI thread and `flush()` runs after completions and input
  are drained, so every component sees the same value in a frame (no tearing, which is the problem
  React's `useSyncExternalStore` solves for concurrent rendering).
- Cost: one intrusive link per subscription, allocated at mount; no allocation on `set()`; the
  subscription set is fixed at mount, so there is no dynamic dependency tracking (the reason signals
  were rejected in ADR-050 section 6).
- (c) over (b): an explicit call follows the rules of hooks, makes the subscription visible in
  source, and avoids subscribing because of a read buried in a callback. It matches Compose's
  `collectAsState()` and Flutter's `ValueListenableBuilder` in being an explicit opt-in.
  (a) stays available and is still right for derived values computed in C++.

**Rules.** `useObservable` follows the rules of hooks; its argument must be a direct member access
on a `useService` result (no variables, no conditionals); the returned value is read-only in TSX;
writes go through service methods from handlers or effects. `useObservable` becomes an entry in the
built-in hook manifest ([`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 7.1) with a new
lowering kind, like `useService`.

**Risk.** Copying large `T` values into render. Rule: render reads through the pointer, no copy;
`T` should be a value type or an immutable snapshot handle.

**Revisit when** computed values (`Computed<T>`, deferred in ADR-050) are needed in TSX.

## 7. Startup spec revision

**Recommendation: agree to all three parts, with one change to the hooks part.**

1. **Owner of P1-P5.** `Application` sequences P1-P5 and detects the launch mode. `AppHost` keeps
   its implemented meaning (the per-window frame loop, [`app-host.md`](app-host.md)) and presents
   P2 for its window. Mapping: P0 platform; P1 `Application` creates the loop, executors, device
   and main window, and starts `StartPhase::Runtime` services; P2 the main window's `AppHost`
   presents the loading subtree; P3 `StartPhase::Background` services start with the critical set,
   auth, restore and route resolution; P4 gates on required services `Started`, the critical set,
   route/restore and the service-requirement validation of ADR-050 section 3 item 6; P5 starts
   `StartPhase::Idle` services and prefetch.
2. **Lifecycle API in V1 (C++).** Yes: `Application::state()`, `Service::on_app_state`,
   `on_terminate_requested` and `TerminationDeferred`. Per-phase developer callbacks stay a
   non-goal: a developer's startup work is a service with a `StartPhase`, which is the one
   extension point.
3. **JSX lifecycle.** Instead of new hooks (`useAppState()`, `useLaunchMode()`), core provides a
   built-in service interface `AppLifecycle` with observables `state` and `launch` (mode and
   activation source). TSX reads it with `useObservable(useService<AppLifecycle>().state)`. This
   adds no hooks beyond `useService` and `useObservable`, in line with
   [AGENTS.md](../../AGENTS.md#non-negotiables) rule 8. `useStartupStage()` stays deferred: the
   loading subtree is static in V1 and is not a service consumer.

**Also fix in ADR-050.** Section 2 maps `Launching` to "P0-P3". App state (visibility and focus)
and startup stage (P0-P5) are orthogonal: during P3 the window may be focused and should be
`Active`. Proposed: `Launching` lasts until the main window is presentable (end of P1); the stage
is exposed separately.

**Revisit when** a custom loading subtree needs live stage text (that is the S8 hook).

## 8. Default module set for the app template

**Criteria proposed for a default module:** used by most apps with more than one screen; needs no
permission, entitlement or network back end; adds no new third-party dependency; and its size delta
after section garbage collection is measured in CI against the template build.

**Recommendation: `storage`, `navigation`, `restore`, `i18n`; `data` becomes opt-in.**

- `storage` (settings and atomic documents) is the base for `restore` and for user preferences.
- `navigation` and `restore` are presupposed by [`startup-api-v1.md`](../specs/startup-api-v1.md)
  (`restoreState`, route resolution before P4, process restore).
- `i18n` costs little because ICU is already linked for text segmentation
  ([`dependencies.md`](../specs/dependencies.md) section on text).
- `data` (the [`async-data-v1.md`](../specs/async-data-v1.md) query cache) is only useful with a
  fetcher, which in practice means `net`, itself opt-in; a default `data` without `net` is an
  engine with nothing to drive it. It is the first module to add when an app enables `net`
  (`tessera new --template networked` could enable both).
- `shell` stays opt-in: text editing does not need it because the clipboard is a HAL seam
  (`include/tessera/hal/clipboard.hpp`), and tray, menus and dialogs vary per app.

**Trade-off accepted.** Apps that fetch data add two lines to `tessera.json`.

**Revisit when** the size measurement shows a default module above an agreed budget, or most
sample apps enable `data`.

## 9. Worker-rendered native surfaces and direct OS sub-surfaces

**Recommendation: neither in V1.**

- **Worker rendering.** [`gpu/rhi.md`](gpu/rhi.md) section 6 states the RHI is used "from one
  thread at a time until the render-thread work lands"; that work (a B2 item) is the precondition,
  not ADR-049, which defines versioning, shader ids and tiers but no threading rules. V1 renders on
  the UI thread in the record step, with a per-surface timing counter so slow renderers are visible.
- **Seam to leave now:** the `SurfaceRenderer::on_render` contract says the renderer may touch only
  its own resources and the `SurfaceRenderContext` (no scene, no services, no observables); all
  inputs reach it through `on_resize`, `on_input` or values the developer class copies in before
  `request_redraw()`. With that rule, moving `on_render` to a worker later changes who calls it,
  not what it may do. The host already treats the surface as a texture it composites (`offscreen`
  default), so the three-target ring is internal.
- **Direct OS sub-surfaces** (overlay planes, child swapchains, `SurfaceView`, `CAMetalLayer`
  children): out of V1, and when added, a per-backend extension under ADR-049 section 4 (Tier 2 or
  3), not a core contract. They break clips, transforms, opacity and z-order inside the scene and do
  not exist in Wasm. Flutter offers both paths (texture widgets and platform views, the latter with
  composition costs), which is evidence that the texture path should stay the default.

**Risk.** A heavy 3D renderer on the UI thread drops UI frames; mitigated by the counter and by
`redraw="onDemand"` as the default.

**Revisit when** (worker) the B2 render-thread work lands or a renderer's recorded cost exceeds a
frame-budget threshold in a sample app; (sub-surface) an app needs protected video playback,
hardware video overlays or an external engine that must own its swapchain.

## 10. Contradictions the recommendations would require fixing

| # | Where | Contradiction | Fix |
|---|---|---|---|
| 1 | ADR-050 section 2 state table | `Launching` = P0-P3 conflates app state with startup stage | `Launching` until the main window is presentable; stage exposed separately (section 7) |
| 2 | ADR-050 section 7 item 4; [`native-render-surfaces.md`](native-render-surfaces.md) section 5 | cite "ADR-049 RHI threading rules"; ADR-049 has none | point to [`gpu/rhi.md`](gpu/rhi.md) section 6 and the B2 render-thread work |
| 3 | `include/tessera/app/window.hpp`, `src/app/window_impl.hpp`, `src/app/window.cpp`; [`app-host.md`](app-host.md) section 5; ADR-020 device ownership | each `WindowHost` owns a device and a `RuntimeLoop`, and `run()` blocks, while ADR-050 has one `Application` loop with timers and several windows | one loop and device per `Application`, per-window swapchain and `pump()`; `run()` kept as a wrapper (section 3) |
| 4 | [`startup-api-v1.md`](../specs/startup-api-v1.md) sections 1, 8, 10 | "no lifecycle API in V1"; "`AppHost` owns P1-P5"; launch-info hooks deferred | `Application` owns P1-P5; C++ lifecycle API is V1; JSX reads `AppLifecycle` through `useService`/`useObservable` (section 7) |
| 5 | [`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 7.1 | the built-in hook manifest has no lowering kind for a type argument checked against a project manifest | add `useService` and `useObservable` entries with service lowering kinds; `ui/services.json` is project data, not new hooks |
| 6 | ADR-050 section 3.1 item 4 and section 6 | reading observables in render is "open" | resolved by `useObservable` (section 6.2) |
| 7 | [`os-events-v1.md`](../specs/os-events-v1.md) section 8 | iOS `sceneDidEnterBackground` maps to `WindowShown/Hidden` only | add the `AppForeground`/`AppBackground` mapping ADR-050 section 2 introduces |
| 8 | [`platform-shell-v1.md`](../specs/platform-shell-v1.md) sections 1 and 6 | tray is V1 but its visibility/battery policy is unspecified | the `Hidden` state and `BackgroundPolicy` of ADR-050 section 5 are that policy |
| 9 | [`dependencies.md`](../specs/dependencies.md) section 7, open question 6, and the abstraction table (`libcurl` behind `services::HttpClient`) | presents libcurl plus mbedTLS as the networking stack | platform back ends, libcurl for Linux only; module names move from `services/*` to `tessera_appfw_*` |
| 10 | [`services.md`](services.md) sections 7 and 10 | "background execution policy on mobile" open | answered by section 1 (later, with seams) |
| 11 | [`startup-api-v1.md`](../specs/startup-api-v1.md) section 5.1 | activation sources are `none`, `deep_link`, `notification` | reserve `background_task` (not V1) |
| 12 | [`navigation-v1.md`](../specs/navigation-v1.md) | does not say whether off-stack screens stay mounted | must be decided before route scopes are reconsidered (section 2) |
| 13 | ADR-050 section 1 rule 3 and section 8 | `data` listed as a template default | opt-in (section 8) |

## 11. What this document does not decide

It does not change any spec, and it does not schedule work beyond the AF slices of ADR-050. Size
estimates (SQLite) and platform details marked unverified must be measured or checked in the slice
that implements them.
