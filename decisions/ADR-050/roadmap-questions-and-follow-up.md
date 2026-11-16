# Roadmap, owner questions, and follow-up

Part of the [ADR-050 decision set](README.md). Section bodies here: `## Roadmap` and `## Questions for the owner` (with the accepted answers) and `## Follow-up`. The heading
numbers, levels and order are the original ones, so `ADR-050 section N` still names the same
heading. Each part is a verbatim slice of the former single file; the shared front matter
(status, date, related decisions, terms) is in the [index](README.md).

## Roadmap (each slice: behavior, tests, docs; headless first)

| Slice | Work | Acceptance (headless unless stated) |
|---|---|---|
| AF1 | `ServiceId`, `ServiceContext` (provide/get/find, freeze, scopes, in-frame lookup counter), `Service` state machine with ordered async start/stop driven by `CompletionQueue`, per-service and total deadlines | with `ManualClock` + `ManualExecutor`: dependency order, cycle and missing-dependency errors, reverse stop, deadline -> `Abandoned`, exception in `start` -> `Failed`, no completion runs after `Stopped`, no allocation on `get`, in-frame lookup counter zero |
| AF2 | `TaskScope` (spawn, cancel, drained, bounded close) and the `RuntimeLoop` timer wake source | timers fire in order under `ManualClock`; idle loop parks until the next deadline; closing a scope cancels and drains |
| AF3 | `Application` lifecycle state machine from scripted normalized events; termination request with `Defer`; `WindowHost` forwards `Focus`/`Blur`/`Minimized`/`Occluded`/`Restored` and honors the section 5.8 pause policy | event scripts produce the state table of section 2; a deferred close does not end `run()`; occluded window presents nothing |
| AF4 | `Observable<T>` + `Subscription` over `UpdateTicket`/`UpdateScheduler` | one flush per tick for many `set()` calls; unsubscribe on unmount; no allocation on `set` |
| AF5 | Codegen: opaque `app_context` through mount, `Requires` + `service<T>()` in `Base<Self>`, `ui/services.json` + `services.d.ts`, `useService<T>()` lowering, manifest union, startup validation (after ADR-040 codegen lands) | generated fixture compiles; undeclared service and non-interface `T` are compile errors with TSX location; missing provider fails before P4; mount resolves each service once |
| AF6 | `NativeSurface` intrinsic, `useRef<NativeSurface>` lowering, `SurfaceRenderer` host on the UI thread with offscreen composition, on-demand and continuous redraw, device-loss callbacks | null RHI records renderer commands; idle surface presents nothing; resize/visibility callbacks under scripted events; lavapipe golden of a composited surface |
| AF7 | `storage` module: settings key/value and atomic writes on `IoExecutor` with an in-memory file-system fake; contract suite | atomicity, migration, corruption fallback against the fake |
| AF8+ | navigation, restore, data, i18n, shell, permissions, notifications as modules per their specs; app template default links; Android lifecycle mapping; worker-rendered surfaces | per spec |

AF1 is the smallest first slice: it touches no existing target except a new `tessera_appfw_core`
library and its unit test executable.

## Questions for the owner

Decided (2026-11-12, see Owner decisions):

- **Direction** (was 1): application framework; layers are an engineering mechanism; the app
  template links `appfw_core` and the default modules. The library prefix `tessera_appfw_*` is kept
  unless the owner objects.
- **TSX surface** (was 2): `useService<Interface>()` only, resolved once at mount; no `useStore`.
- **Virtual dispatch for services** (was 9): acceptable at the service boundary and for service
  interfaces (measured); per-frame calls use `final` types.
- **Resolution mechanism**: frozen registry, resolve once at mount, never in the frame path
  (section 3).

### Answers to open questions (accepted by the owner, 2026-11-12)

Accepted as proposed. Each item gives the question and the **answer** (options,
trade-offs, seams, risks, revisit triggers and the spec contradictions to fix are in
[`app-framework-open-questions.md`](../../docs/design/app-framework-open-questions.md)):

1. **OS-scheduled background work; tray apps.** Accepted: WorkManager/BGTaskScheduler later (Big
   Step F); tray-resident desktop apps in V1 (`Hidden` with zero windows); seams: window-less
   `Application` start and a `BackgroundTask` interface.
2. **Route-scoped services.** Accepted: app and window scopes only; per-route state in
   `NavEntry.state`; scopes built as a parent chain so a route level is additive.
3. **Multi-window.** Accepted: yes on desktop in V1, as its own slice after one loop and one device
   per `Application` replace today's per-window device, loop and blocking `run()`.
4. **Database.** Accepted: in-tree opt-in SQLite module (vendored amalgamation, I/O executor only),
   not default-linked; ORMs and other engines community.
5. **Networking.** Accepted: platform stacks in-tree (WinHTTP, NSURLSession, `fetch`, Android
   later), libcurl only on Linux, one contract suite.
6. **State model.** Accepted: plain observables plus a debug change trace, no action log; TSX reads
   observables in render only via `useObservable(svc.member)`, a mount-time subscription.
7. **Startup spec.** Accepted: `Application` owns P1-P5; C++ lifecycle API in V1; JSX reads a
   built-in `AppLifecycle` service instead of new lifecycle hooks; `Launching` ends with P1.
8. **Default modules.** Accepted: `storage`, `navigation`, `restore`, `i18n`; `data` opt-in.
9. **Native surfaces.** Accepted: no worker rendering or OS sub-surfaces in V1; restrict
   `on_render` now so it can move to a worker later.

## Follow-up (if accepted)

1. Fix the dependency direction in [`module-map.md`](../../docs/architecture/module-map.md) section 2
   and the layer table in [`overview.md`](../../docs/architecture/overview.md) section 5.
2. Rewrite [`services/common-contract.md`](../../docs/design/services/common-contract.md) section 1, [`services/background-and-lifecycle.md`](../../docs/design/services/background-and-lifecycle.md) section 7 and [`services/status-and-open-questions.md`](../../docs/design/services/status-and-open-questions.md) section 9 around sections 3-5 of
   this ADR; add `docs/design/app-framework.md` (lifecycle, scopes, provisioning, stores, surfaces).
3. Revise [`startup-api-v1.md`](../../docs/specs/startup-api-v1.md) sections 1, 8 and 10 (owner of
   P1-P5, C++ lifecycle API) and [`component-model-v1.md`](../../docs/specs/component-model-v1.md)
   section 12 (dependency injection resolved by this ADR); add `useService` and
   `useRef<NativeSurface>` to the hook manifest and the JSX subset spec.
4. Add `AppForeground`/`AppBackground` to [`os-events-v1.md`](../../docs/specs/os-events-v1.md) and
   the timer wake source and shutdown budget fields to
   [`runtime-config-v1.md`](../../docs/specs/runtime-config-v1.md).
5. New specs: `services-manifest-v1.md` (`ui/services.json`), `native-surface-v1.md`,
   `permissions-v1.md`, `storage-v1.md`, `notifications-v1.md`, and later the headless background
   entry. Amend ADR-028's follow-up to share the `SurfaceRenderer` contract.
6. Fix the stale texts listed in section 3 of the audit (protected regions in component-model and
   overview, I/O executor status).
