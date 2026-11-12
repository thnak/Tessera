# ADR-050: Opt-in application-framework layer above the UI runtime (services, lifecycle, task scopes, app state)

- Status: Proposed
- Date: 2026-11-12
- Related: [ADR-012](ADR-012-static-linking.md) (static linking),
  [ADR-013](ADR-013-project-license-mit.md) (MIT),
  [ADR-021](ADR-021-async-runtime.md) (async runtime),
  [ADR-023](ADR-023-os-event-normalization-and-foldables.md) (OS events),
  [ADR-025](ADR-025-error-boundaries.md) (error boundaries),
  [ADR-030](ADR-030-app-platform-completeness.md) (navigation, restore, shell, i18n, data),
  [ADR-040](ADR-040-generated-base-and-developer-class.md) (generated base + developer class),
  [ADR-042](ADR-042-jsx-startup-surface.md) and [ADR-043](ADR-043-launch-modes-and-state-reuse.md)
  (startup surface, launch modes), ADR-049 (backend extensibility, open as
  [PR #83](https://github.com/thnak/Tessera/pull/83)),
  [`app-framework-gap-audit-v1.md`](../docs/architecture/app-framework-gap-audit-v1.md) (the audit
  this ADR answers), [`services.md`](../docs/design/services.md),
  [`async.md`](../docs/design/async.md).
- Terms: **core** = the UI runtime that exists today (`tessera_ui`, `tessera_runtime`,
  `tessera_app` host and window driver, rendering, HAL). **App framework** = the opt-in layer this
  ADR proposes. **Service** = a long-lived app-scoped object with a start/stop lifecycle (storage,
  navigation, data, ...). **Composition root** = the code that creates the services and windows
  (the app's `main()` or the generated entry). **Task scope** = an owner of a group of tasks that
  can cancel them and knows when they have all finished.

## Context

The audit ([`app-framework-gap-audit-v1.md`](../docs/architecture/app-framework-gap-audit-v1.md))
finds that the frame loop, input, style, layout and async primitives are implemented, but the
structure that Qt, JUCE, Flutter and Jetpack give every app is missing or contradictory:

1. **No app object or app lifecycle.** `WindowHost::run()` ends on `Close`; `Focus`, `Blur`,
   `Occluded`, `Minimized` and `Restored` are dropped; there is no termination veto and no
   app-level foreground/background state. [`startup-api-v1.md`](../docs/specs/startup-api-v1.md)
   rules out a lifecycle API in V1 while other specs need services to react to suspend/resume.
2. **No service model.** [`services.md`](../docs/design/services.md) defines only a synchronous
   `shutdown()`; nothing defines start order, failure, or bounded asynchronous stop.
3. **No way for components to reach app-scoped objects.** Context is rejected by
   [`component-model-v1.md`](../docs/specs/component-model-v1.md) (its section 12 leaves
   "context-free dependency injection" open), yet `NavController`, `DataStore`, `ShellService`
   and `RestoreService` are specified as if components can call them.
4. **No app-wide state pattern** beyond component-local `useState` and local stores.
5. **Dependency direction is wrong on paper.** [`module-map.md`](../docs/architecture/module-map.md)
   makes the runtime depend on services; services need the runtime's executors and events, so
   they must sit above it.

The owner did not set an application-framework direction when the project started. This ADR
proposes one that keeps the UI core usable on its own (embedded in another app, a kiosk, a plugin
host, a test) and makes every app-framework piece optional, while respecting the hard rules in
[`CLAUDE.md`](../CLAUDE.md): no RTTI or hidden allocations on hot paths, never block the UI thread,
no detached tasks, exceptions only at boundaries, static linking, MIT.

## Decision

### 1. Scope: three tiers, each piece separately linkable

| Tier | Contents | May depend on | Must not contain |
|---|---|---|---|
| 0. Core UI runtime (exists) | scene, style, layout, text, animation, input, `AppHost`, `WindowHost`, `RuntimeLoop`, executors, `CompletionQueue`, clock, logger, HAL | `core`, HAL, RHI | service registry, stores, app lifecycle states, navigation, storage |
| 1. App framework core (`tessera_appfw_core`) | `Application`, lifecycle state machine, `ServiceContext`, `Service` base, `TaskScope`, timers, `Observable<T>` | Tier 0 | platform APIs (it consumes normalized events only) |
| 1. App framework modules (`tessera_appfw_<name>`) | storage, navigation, restore, data, shell, i18n, permissions, notifications, net, updates | `tessera_appfw_core`, Tier 0, HAL; another module only when declared in its spec | UI internals; another module's private headers |
| 2. Community adapters | providers behind module interfaces (HTTP backends, database, push, OAuth, analytics, cloud sync), extra platforms | the public module interfaces | changes to Tier 0/1 contracts |

Rules:

1. Tier 0 gains **seams, not concepts**: it forwards every normalized window/OS event (today it
   drops most), exposes a timer wake source (section 5), and carries one opaque app-context pointer
   through component mounting (section 3). It never names a service, store or app state.
2. Each Tier 1 module is its own static library behind a CMake option
   (`TESSERA_APPFW_<NAME>`, default ON for the app template and OFF for the core-only presets), so
   an app links only what it uses. No module is required by Tier 0, the compiler or the goldens.
3. The dependency direction becomes `core <- runtime <- appfw_core <- appfw modules <- app`. The
   module map and overview are corrected accordingly (Follow-up).
4. An app that does not use the framework keeps writing its own `main()` with `WindowHost`, as
   `samples/click-demo` does today.

### 2. Application object and app lifecycle

`tessera::appfw::Application` is created by the composition root on the UI thread. It owns the app
service scope, the lifecycle state machine, the root task scope, the timer queue and the shutdown
sequencer. It borrows the clock, `RuntimeLoop`, `CompletionQueue` and executors, so the same object
runs headless with `ManualClock` and `ManualExecutor`. Windows register with it; each window gets a
child service scope (section 3).

App states, derived only from normalized events:

```text
Launching -> Active <-> Inactive <-> Hidden <-> Suspended
any state except Terminated -> Terminating -> Terminated
```

| State | Meaning | Desktop source | Android source | iOS source |
|---|---|---|---|---|
| `Launching` | P0-P3 of [`startup-api-v1.md`](../docs/specs/startup-api-v1.md) | process start | `onCreate` | `scene(_:willConnectTo:)` |
| `Active` | visible and focused | a window focused | `onResume` / top-resumed | `sceneDidBecomeActive` |
| `Inactive` | visible, not focused | all windows blurred | `onPause` (multi-window) | `sceneWillResignActive` |
| `Hidden` | no visible window; process runs | all windows minimized/occluded/closed with `quitOnLastWindowClosed = false` | `onStop` | `sceneDidEnterBackground` |
| `Suspended` | process will be frozen or may be killed | OS sleep (`Suspend`) | process cached after `onStop` + `onTrimMemory` | background grace expired |
| `Terminating` | shutdown sequence running (section 4) | last window closed or `ShutdownRequested` | `onDestroy` of the last activity / finish | `applicationWillTerminate` |

- [`os-events-v1.md`](../docs/specs/os-events-v1.md) gains `AppForeground`/`AppBackground`
  (process-level visibility, distinct from a window's `WindowHidden` and from `Suspend`); the
  state machine never reads platform types.
- **Termination request.** Closing the last window or `ShutdownRequested` asks every started
  service `on_terminate_requested()`; a service answers `Allow` or `Defer` (unsaved work), which
  surfaces to the app as a `TerminationDeferred` event so it can show a confirmation. The request
  is asynchronous and bounded; the UI thread never waits. On mobile the OS cannot be vetoed; there
  the request only triggers checkpoints.
- **Startup.** `Application` drives P1-P5 and the launch modes of ADR-042/ADR-043. The startup spec's
  section 8 name "`AppHost` owns the P1-P5 sequence" is corrected: the frame-loop `AppHost` keeps
  its current meaning, and the startup sequence belongs to `Application`.
- This revises the V1 non-goal of [`startup-api-v1.md`](../docs/specs/startup-api-v1.md) for C++
  only: services and app code observe the lifecycle through `Application`. JSX hooks
  (`useLaunchMode()`, `useAppState()`) stay deferred until the hook manifest exists.

### 3. Service provisioning without RTTI

Options:

| Option | How a component gets `Navigator&` | For | Against |
|---|---|---|---|
| A. Constructor injection per dependency through generated parents | each generated `Base<Self>` takes the services it and its subtree need | fully static | every ancestor's constructor changes when a leaf needs a service; codegen must compute transitive dependency sets; parent components carry dependencies they do not use |
| B. Typed `ServiceContext` keyed by compile-time ids | `context.get<Navigator>()` resolved once at mount and cached | no RTTI, no reflection; fakes are trivial in tests; works for generated and hand-written code | a missing service is a run-time error unless validated early |
| C. Global singletons / function-local statics | `Navigator::instance()` | no plumbing | hidden global state ([`services.md`](../docs/design/services.md) forbids it); breaks test isolation and multi-window |
| D. React-style context through the scene tree | `useContext(NavContext)` walks ancestors | familiar to React users | rejected by the component model; tree walk per lookup; needs IR support |
| E. Compile-time DI library (for example Boost.DI) | constructor graph generated by templates | static wiring | new dependency; long compile times and template diagnostics; still needs a way into generated components |

**Decision: B as the mechanism, reached through one opaque pointer in the mount path, with
requirements declared on the developer class and validated at startup.**

```cpp
namespace tessera::appfw {
struct ServiceId { std::uint64_t value; };               // FNV-1a of a reverse-DNS name
consteval ServiceId service_id(std::string_view name);

template <typename T>
concept ProvidedService = requires { { T::kServiceId } -> std::convertible_to<ServiceId>; };

class ServiceContext {                                   // app scope or window scope
public:
    template <ProvidedService T> std::expected<void, ServiceError> provide(T& service);
    template <ProvidedService T> T& get() const;          // required: validated at startup
    template <ProvidedService T> T* find() const;         // optional service, may be null
};
}

// Developer class (ADR-040) declares what it uses; the base only exposes those.
class Inbox final : public gen::InboxBase<Inbox> {
public:
    using Requires = tessera::appfw::requires_services<Navigator, MailStore>;
    void open_message(std::uint64_t id) { service<Navigator>().push(routes::message(id)); }
};
```

1. **Keys.** `ServiceId` is a `consteval` 64-bit hash of a reverse-DNS name (`"tessera.nav"`,
   `"com.example.mail"`), declared as `static constexpr ServiceId kServiceId` on the service type.
   The registry stores `{id, type tag, pointer}`; the type tag is the address of an `inline`
   variable template instantiated per type, so `get<T>()` checks both id and tag with two integer
   compares. No `typeid`, no `dynamic_cast`. A duplicate id or a tag mismatch is an error at
   `provide()`, not undefined behavior.
2. **Storage.** A fixed-capacity flat array (default 64 entries per scope, configurable), sorted
   and frozen when the application starts; lookup is a binary search. Lookups are not on hot paths:
   a component resolves its services in `mount()` and keeps references.
3. **Scopes.** Two levels in V1: app scope and window scope; a window scope falls back to the app
   scope. Route-scoped services are deferred.
4. **Reaching components.** Tier 0's mount path carries one opaque `void* app_context` slot (like
   `UpdateTicket::context`), set by the window and passed by each generated parent to its children
   unchanged. The ADR-040 base reinterprets it as `const ServiceContext*` only when the developer
   class declares `Requires`; `service<T>()` is a `static_assert` error for a `T` not listed in
   `Requires`. Components without `Requires` generate no code for this.
5. **Fail at startup, not on first click.** Codegen emits each component's `Requires` into the
   generated manifest; `Application` checks the union against the frozen scopes before P4 and
   reports every missing service with the component and TSX location
   (`TESSERA_APPFW_SERVICE_MISSING`, routed to the root error boundary).
6. **Tests** build a fresh `ServiceContext`, provide fakes under the same ids and mount the
   component; there is no global state to reset.
7. **TSX surface.** None in the first slices: services are used from developer-class handlers. A
   later `useService(Navigator)` hook lowers to the same `service<T>()` call once the hook
   manifest exists (owner question 2).

### 4. Service lifecycle

```cpp
class Service {                       // virtual dispatch is fine here: tens of objects, rare calls
public:
    virtual ~Service() = default;
    virtual runtime::Task<Status> start(StartContext& ctx) = 0;   // ctx: scope, token, deps
    virtual runtime::Task<void> stop(StopContext& ctx) = 0;       // bounded by ctx.deadline
    virtual void on_app_state(AppState state) {}                  // UI thread, must not block
    virtual TerminateVote on_terminate_requested() { return TerminateVote::Allow; }
};
```

```text
Registered -> Starting -> Started -> Stopping -> Stopped
                  \           \          \
                   +-> Failed  +-> Failed +-> Abandoned (stop deadline expired)
```

1. **Order.** A service declares `kDependsOn` (a list of service ids). `Application` sorts services
   topologically at freeze time; a cycle or a dependency on an unregistered id is a startup error.
   Stop runs in reverse order.
2. **Phases.** Each service declares `StartPhase::Runtime` (P1, cheap, needed for the first
   frame, for example the logger sink), `StartPhase::Background` (P3, the default) or
   `StartPhase::Idle` (P5). A service marked `required` gates P4; an optional one does not.
3. **Ownership of work.** Each service gets its own `TaskScope` (section 5). Everything a service
   starts belongs to that scope; when the service reaches `Stopped`, its scope is cancelled and
   drained, and completions tagged with its token are dropped instead of running.
4. **Bounded, non-blocking shutdown.** `Terminating` runs a state machine pumped by `RuntimeLoop`
   ticks: stop services in reverse order, each with a per-service budget (default 500 ms) inside a
   total budget (default 2 s, a `runtime-config-v1.md` field). A service past its deadline is
   cancelled and marked `Abandoned` with a diagnostic; shutdown continues. Nothing calls `join()`
   or waits on the UI thread; worker pools are shut down last under their own bounded drain.
5. **Errors.** `start` returns a `Status` value. An exception escaping `start`, `stop` or a
   callback is caught at the service boundary (ADR-025 rule: exceptions at boundaries), turned
   into `Failed` plus a diagnostic, and never propagates into the frame loop. A failed required
   service routes to the root error boundary (same path as `TESSERA_STARTUP_CRITICAL_FAILED`); a
   failed optional service leaves `find<T>()` returning null.

### 5. Background work: task scopes, timers, desktop and mobile

1. **`TaskScope`** (Tier 1, built on the existing `Task`, `CancellationSource` and
   `CompletionQueue`): `spawn(task)`, `cancel()`, `drained()`, a bounded task count, and a
   deadline-bounded `close()` used by the shutdown sequencer. Scopes nest: app, service, window,
   component (unmount cancels). This is how "no detached tasks" becomes enforceable: the app
   framework offers no way to start a task without a scope.
2. **Timers** are a Tier 0 wake source: `RuntimeLoop` gains a bounded timer queue whose earliest
   deadline joins the pacer deadline, so an idle app parks until the next timer instead of polling.
   One-shot and periodic; a periodic timer coalesces missed periods; timers are deterministic under
   `ManualClock`. A timer never forces a frame; its callback schedules work like any completion.
3. **Desktop.** `Hidden` stops rendering (the `runtime-config-v1.md` section 5.8 policy, which the
   window driver must start honoring) but services keep running under their
   `BackgroundPolicy { Continue, Throttle, Pause }` (default `Continue`). A tray-resident app sets
   `quitOnLastWindowClosed = false` and stays `Hidden`.
4. **Mobile.** `Hidden` asks every service to checkpoint (`on_app_state`); `Suspended` pauses all
   timers and cancels scopes whose policy is not `Continue`; in-flight I/O may finish only within
   the platform grace period, then it is cancelled. No rendering and no GPU work while not visible.
5. **OS-scheduled work** (Android WorkManager/JobScheduler, iOS BGTaskScheduler, Android
   foreground services) runs outside the visible app lifetime. The contract is a **headless
   entry**: the same `Application` started without windows, with a declared subset of services,
   under the OS time budget. It is deferred to its own spec after the Android backend exists
   (owner question 4).

### 6. App-wide state

Options: (a) component-local state and props only (status quo); (b) context propagation through the
tree; (c) observable values owned by services; (d) a reducer/action store (Redux style); (e) a
signals graph with automatic dependency tracking.

**Decision: (a) stays the default; app-wide state is (c).**

- `Observable<T>` (Tier 1) holds a value and a version on the UI thread. `subscribe(UpdateTicket&)`
  returns an RAII `Subscription`; `set()` compares with `operator==` and, on change, calls
  `schedule()` on each subscribed ticket. Updates therefore reuse the existing `UpdateScheduler`
  batching: no synchronous re-render, one flush per tick, per-window schedulers work unchanged.
- A **store** is a service that owns observables and the methods that change them. Writes happen on
  the UI thread; background results arrive through the completion queue and then call `set()`.
- Subscriber storage is allocated at `subscribe()` (mount), never at `set()`; notifying is a loop
  over intrusive links.
- `NavController`, the async-data `DataStore` and `PermissionService` expose their state as
  observables, so every screen subscribes the same way.
- Rejected as the mandated pattern: (b) for the component-model reasons in section 3; (d) because
  reducers need switch-heavy code and an action log the V1 surface rejects (`useReducer` is unsafe
  in [`react-compat.md`](../docs/specs/jsx-intrinsics-v1/react-compat.md)), though an app may build
  one on top of (c); (e) because automatic tracking hides allocations and ordering. Derived values
  (`Computed<T>`) are deferred.

### 7. Module placement (each module keeps or gets its own spec)

| Module | In-tree scope | Community scope |
|---|---|---|
| `storage` | settings key/value with versioned schema, atomic document writes, cache directory, migrations, keystore interface + Windows/Linux implementations | database adapters (SQLite and others), cloud sync |
| `navigation`, `restore`, `data`, `shell`, `i18n` | as specified by [ADR-030](ADR-030-app-platform-completeness.md) and its specs, now as modules | extra platform implementations |
| `permissions` (new spec) | state model (`NotDetermined`, `Granted`, `Denied`, `DeniedPermanently`) and just-in-time request flow | per-platform permission back ends beyond Tier 1 |
| `notifications` | local notifications, activation routing | push providers (FCM, APNs, Windows push) |
| `net` | HTTP client interface, cancellation/timeout/retry rules, deterministic fake | transport back ends (pending owner question 7) |
| `updates` | the signed-manifest reference flow of [`updates-and-distribution-v1.md`](../docs/specs/updates-and-distribution-v1.md) | store-specific and MDM updaters |

### 8. In-tree versus community

The tiers of ADR-049 apply unchanged to app-framework providers: Tier 1 in-tree and release
blocking (`appfw_core`, every module interface, its fake, and the Windows/Linux implementations);
Tier 2 community-maintained with a named maintainer and a passing contract suite; Tier 3
experimental, out of tree or behind an OFF option. Each module interface ships a **contract test
suite** run against its fake, which a provider links and runs (the app-framework equivalent of
ADR-049's backend conformance suite). Contributed providers follow ADR-049's licensing rules: MIT
code, static linking, no GPL/LGPL, entries in `docs/specs/dependencies.md` and
`THIRD_PARTY_NOTICES.md`. The **contracts** (lifecycle states, `ServiceContext`, `Service`,
`TaskScope`, `Observable`, module interfaces) stay core-owned and change only by ADR or spec.

### 9. Hard-constraint check

| Constraint | How this design meets it |
|---|---|
| No RTTI or reflection | compile-time ids plus per-type address tags; requirements are C++ types checked by `static_assert` |
| No hidden allocations on hot paths | fixed-capacity registries frozen at start; lookups at mount; subscriptions allocate at mount only |
| Never block the UI thread | async `start`/`stop`; shutdown and termination requests are tick-driven state machines |
| No detached tasks | tasks start only inside a `TaskScope`; scopes close under a deadline |
| Exceptions only at boundaries | the service boundary catches and converts to `Failed`; values (`Status`, `std::expected`) elsewhere |
| Static linking, MIT | every tier is a static library; providers follow ADR-049 section 5 |

## Consequences

Positive:

- The core stays a UI runtime that can be embedded or tested alone; app concepts arrive only when
  linked.
- One answer to "how does a component reach X" for navigation, data, shell, restore and app code,
  checked at compile time (declared `Requires`) and at startup (provided services).
- Lifecycle, background and shutdown behavior become testable headlessly: scripted events,
  `ManualClock`, `ManualExecutor`.
- The specs written for ADR-030 get a shared runtime shape instead of each inventing access,
  ownership and shutdown.

Negative and risks:

- A new layer to maintain, and the [`startup-api-v1.md`](../docs/specs/startup-api-v1.md),
  [`services.md`](../docs/design/services.md), module-map and component-model texts must be revised.
- Missing services are found at startup, not at compile time: the composition root is ordinary C++
  and cannot be checked against every component's needs statically without making the app type a
  template parameter everywhere.
- The opaque `app_context` pointer is untyped inside Tier 0; misuse is prevented only by the
  generated base being the sole reader.
- The service-provisioning codegen depends on ADR-040's `Base<Self>` codegen, which is not
  implemented yet.
- Virtual dispatch for `Service` departs from ADR-040's CRTP-only style; it is limited to the
  service boundary.

## Alternatives considered

- **Stay a UI toolkit; leave app structure to each app.** Rejected: four accepted specs already
  assume app-scoped services, and every app would re-solve lifecycle and shutdown under the same
  hard constraints.
- **Put services in the core runtime** (what the current module map implies). Rejected: forces
  every embedder to link storage, navigation and their dependencies, and inverts the layering.
- **Qt-style meta-object system** (signals, properties, runtime lookup by name). Rejected: needs
  reflection or a moc-like generator for every C++ type, which the RTTI rule excludes.
- **Constructor injection through generated parents only** (option A), **global singletons**
  (option C), **React context** (option D) and **a DI library** (option E): see section 3.
- **Redux-style store as the only state model** and **signals with automatic tracking**: see
  section 6.

## Roadmap (each slice: behavior, tests, docs; headless first)

| Slice | Work | Acceptance (headless unless stated) |
|---|---|---|
| AF1 | `ServiceId`, `ServiceContext` (provide/get/find, freeze, scopes), `Service` state machine with ordered async start/stop driven by `CompletionQueue`, per-service and total deadlines | with `ManualClock` + `ManualExecutor`: dependency order, cycle and missing-dependency errors, reverse stop, deadline -> `Abandoned`, exception in `start` -> `Failed`, no completion runs after `Stopped`, no allocation on `get` |
| AF2 | `TaskScope` (spawn, cancel, drained, bounded close) and the `RuntimeLoop` timer wake source | timers fire in order under `ManualClock`; idle loop parks until the next deadline; closing a scope cancels and drains |
| AF3 | `Application` lifecycle state machine from scripted normalized events; termination request with `Defer`; `WindowHost` forwards `Focus`/`Blur`/`Minimized`/`Occluded`/`Restored` and honors the section 5.8 pause policy | event scripts produce the state table of section 2; a deferred close does not end `run()`; occluded window presents nothing |
| AF4 | `Observable<T>` + `Subscription` over `UpdateTicket`/`UpdateScheduler` | one flush per tick for many `set()` calls; unsubscribe on unmount; no allocation on `set` |
| AF5 | Codegen: opaque `app_context` through mount, `Requires` + `service<T>()` in `Base<Self>`, manifest union, startup validation (after ADR-040 codegen lands) | generated fixture compiles; undeclared service is a compile error; missing provider fails before P4 with the TSX location |
| AF6 | `storage` module: settings key/value and atomic writes on `IoExecutor` with an in-memory file-system fake; contract suite | atomicity, migration, corruption fallback against the fake |
| AF7+ | navigation, restore, data, shell, permissions, notifications as modules per their specs; Android lifecycle mapping; headless entry spec | per spec |

AF1 is the smallest first slice: it touches no existing target except a new `tessera_appfw_core`
library and its unit test executable.

## Questions for the owner

1. **Direction.** Is Tessera an application framework (this ADR) or a UI toolkit with optional
   helpers? Is the name `tessera_appfw_*` acceptable, and should the app template link the
   modules by default?
2. **TSX surface.** Should V1 expose services and observables to TSX (`useService`, `useStore`
   through the hook manifest), or only to developer-class C++ (ADR-040)?
3. **Scopes.** App and window only in V1, or route-scoped services too?
4. **OS-scheduled background work** (WorkManager, BGTaskScheduler, foreground services): V1 or
   later? On desktop, is a tray-resident app (`quitOnLastWindowClosed = false`) in scope?
5. **Multi-window.** One `Application` with several `WindowHost`s in V1? (The startup spec defers
   several windows per App subtree; `app-host.md` expects one host per window.)
6. **Database.** In-tree SQLite adapter (public domain, statically linkable) or community only?
7. **Networking stack.** Platform HTTP stacks (WinHTTP, NSURLSession, Android) versus a vendored
   one; this decides whether `net` back ends are in-tree.
8. **State model.** Are plain observables enough, or does devtools time-travel
   ([`devtools-protocol-v1.md`](../docs/specs/devtools-protocol-v1.md)) require an action log
   from the start?
9. **Virtual dispatch for services.** Acceptable at the service boundary, or should services also
   be CRTP/concept-based like components?
10. **Startup spec revision.** Agree that `Application` (not the frame-loop `AppHost`) owns P1-P5,
    and that a C++ lifecycle API becomes part of V1 while JSX lifecycle hooks stay deferred?

## Follow-up (if accepted)

1. Fix the dependency direction in [`module-map.md`](../docs/architecture/module-map.md) section 2
   and the layer table in [`overview.md`](../docs/architecture/overview.md) section 5.
2. Rewrite [`services.md`](../docs/design/services.md) sections 1, 7 and 9 around sections 3-5 of
   this ADR; add `docs/design/app-framework.md` (lifecycle, scopes, provisioning, stores).
3. Revise [`startup-api-v1.md`](../docs/specs/startup-api-v1.md) sections 1, 8 and 10 (owner of
   P1-P5, C++ lifecycle API) and [`component-model-v1.md`](../docs/specs/component-model-v1.md)
   section 12 (dependency injection resolved by this ADR).
4. Add `AppForeground`/`AppBackground` to [`os-events-v1.md`](../docs/specs/os-events-v1.md) and
   the timer wake source and shutdown budget fields to
   [`runtime-config-v1.md`](../docs/specs/runtime-config-v1.md).
5. New specs: `permissions-v1.md`, `storage-v1.md`, `notifications-v1.md`, and later the headless
   background entry.
6. Fix the stale texts listed in section 3 of the audit (protected regions in component-model and
   overview, I/O executor status).
