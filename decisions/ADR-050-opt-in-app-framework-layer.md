# ADR-050: Application-framework layer above the UI runtime (lifecycle, services, task scopes, app state, native render surfaces)

- Status: Proposed (revised 2026-11-12 with the owner decisions below)
- Date: 2026-11-12
- File name: kept as `ADR-050-opt-in-app-framework-layer.md` so existing links stay valid; the
  layer is no longer "opt-in" in the product sense (owner decision 1).
- Related: [ADR-012](ADR-012-static-linking.md) (static linking),
  [ADR-013](ADR-013-project-license-mit.md) (MIT),
  [ADR-021](ADR-021-async-runtime.md) (async runtime),
  [ADR-023](ADR-023-os-event-normalization-and-foldables.md) (OS events),
  [ADR-025](ADR-025-error-boundaries.md) (error boundaries),
  [ADR-028](ADR-028-custom-native-elements.md) (custom native elements, low-level rendering),
  [ADR-030](ADR-030-app-platform-completeness.md) (navigation, restore, shell, i18n, data),
  [ADR-040](ADR-040-generated-base-and-developer-class.md) (generated base + developer class),
  [ADR-042](ADR-042-jsx-startup-surface.md) and [ADR-043](ADR-043-launch-modes-and-state-reuse.md)
  (startup surface, launch modes), ADR-049 (backend extensibility, open as
  [PR #83](https://github.com/thnak/Tessera/pull/83)),
  [`app-framework-gap-audit-v1.md`](../docs/architecture/app-framework-gap-audit-v1.md) (the audit
  this ADR answers), [`service-resolve-benchmark.md`](../docs/design/service-resolve-benchmark.md)
  (measurements behind section 3), [`services.md`](../docs/design/services.md),
  [`async.md`](../docs/design/async.md).
- Terms: **core** = the UI runtime that exists today (`tessera_ui`, `tessera_runtime`,
  `tessera_app` host and window driver, rendering, HAL). **App framework** = the layer this ADR
  adds above the core. **Service** = a long-lived app-scoped object with a start/stop lifecycle
  (storage, navigation, data, ...). **Service interface** = an abstract C++ class that names a
  service contract; implementations are concrete C++ classes. **Composition root** = the code that
  creates the services and windows (the app's `main()` or the generated entry). **Task scope** = an
  owner of a group of tasks that can cancel them and knows when they have all finished.
  **Resolve** = turning a service id into a typed pointer.

## Owner decisions (2026-11-12)

Recorded as given; the rest of this ADR is revised to follow them.

1. **Direction.** "Tessera IS an application framework, not just a UI toolkit with optional
   helpers: apps built on Flutter/React Native adopt the framework's programming model, so the
   framework should manage more (lifecycle, services, background work, state, storage,
   navigation). Keep the tiering and opt-in linkability only as an engineering/layering
   mechanism, and revise wording that frames it as 'optional helpers'; reconsider whether the app
   template should link the core appfw modules by default (recommend yes)."
2. **TSX access to services** (this correction supersedes an earlier answer that ruled out TSX
   hooks): "TSX DOES get service access, but only as `useService<T>()` where T must be an
   interface (abstract service contract), never a concrete type or string key; developers still
   implement/register the services in C++. Concrete implementations stay in C++ (developer class
   over gen::<Component>Base<Self>). [...] the hook is compile-time typed, lowered by the
   compiler/codegen to resolve-once-at-mount (typed pointer/ref cached in the component), no
   per-render lookup, no runtime string/type_index lookup; the compiler should reject a T that is
   not a registered interface and report missing providers at startup."
3. **Resolve cost.** "Needs careful study: the service RESOLVE mechanism may affect
   performance. Evaluate it with real numbers [...] Then conclude in the ADR which resolution
   mechanism is chosen and WHERE resolution may happen (mount only; never in the frame/hot
   path)." Measured in [`service-resolve-benchmark.md`](../docs/design/service-resolve-benchmark.md);
   conclusion in section 3 and the appendix.
4. **(3b) Refs bound to native rendering.** "Patterns like native 3D drawing need a `ref`
   mechanism: a component gets a typed ref to a native render surface/host element (e.g. a
   viewport node that the app's C++ renders into through the RHI, with size/DPI/visibility/
   lifecycle callbacks, input forwarding, and damage/dirty integration with the frame pipeline,
   offscreen texture or direct sub-surface composited by the scene graph)." Section 7.

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
6. **No per-instance native render surface.** ADR-028 covers reusable custom elements declared in
   a manifest; an app that draws its own 3D view into one component has no typed handle to it.

The owner has set the direction (decision 1): Tessera is an application framework in the sense of
Flutter or React Native. Apps adopt its programming model, and the framework manages lifecycle,
services, background work, state, storage and navigation. The UI core stays separable as an
engineering property (it can be tested, embedded or ported alone), not as a product promise that
the app pieces are optional extras. The hard rules in [`CLAUDE.md`](../CLAUDE.md) still bind: no
RTTI or hidden allocations on hot paths, never block the UI thread, no detached tasks, exceptions
only at boundaries, static linking, MIT.

## Decision

### 1. Scope: an application framework, built in layers

| Layer | Contents | May depend on | Must not contain |
|---|---|---|---|
| 0. UI core (exists) | scene, style, layout, text, animation, input, `AppHost`, `WindowHost`, `RuntimeLoop`, executors, `CompletionQueue`, clock, logger, HAL, RHI | `core`, HAL, RHI | service registry, stores, app lifecycle states, navigation, storage |
| 1. App framework core (`tessera_appfw_core`) | `Application`, lifecycle state machine, `ServiceContext`, `Service` base, `TaskScope`, timers, `Observable<T>`, native surface host (section 7) | layer 0 | platform APIs (it consumes normalized events only) |
| 1. App framework modules (`tessera_appfw_<name>`) | storage, navigation, restore, data, shell, i18n, permissions, notifications, net, updates | `tessera_appfw_core`, layer 0, HAL; another module only when declared in its spec | UI internals; another module's private headers |
| 2. Community adapters | providers behind module interfaces (HTTP back ends, database, push, OAuth, analytics, cloud sync), extra platforms | the public module interfaces | changes to layer 0/1 contracts |

Rules:

1. Layer 0 gains **seams, not concepts**: it forwards every normalized window/OS event (today it
   drops most), exposes a timer wake source (section 5), carries one opaque app-context pointer
   through component mounting (section 3), and hosts native surface nodes (section 7). It never
   names a service, store or app state.
2. The layers are an **engineering mechanism**: each module is its own static library so that
   dependency direction is enforced by the build, tests link only what they test, and an embedder
   or a port can bring up layer 0 first. They are not a statement that apps should assemble the
   framework themselves.
3. **The app template links the framework by default.** `tessera new` and the generated entry
   link `tessera_appfw_core` and the modules every app relies on: `storage`, `navigation`,
   `restore`, `data` and `i18n`. Modules that pull platform permissions, entitlements or network
   back ends (`permissions`, `notifications`, `net`, `updates`, `shell`) are enabled in
   `tessera.json` per app. Static linking with section garbage collection drops unused code, so a
   default module an app never calls costs little binary size. The core-only CMake presets keep
   every module OFF for layer 0 development and CI.
4. The dependency direction becomes `core <- runtime <- appfw_core <- appfw modules <- app`. The
   module map and overview are corrected accordingly (Follow-up).
5. Hand-written `main()` with `WindowHost`, as in `samples/click-demo`, stays supported for layer 0
   tests and embedding; it is not the documented way to build an app.

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
- **Startup.** `Application` drives P1-P5 and the launch modes of ADR-042/ADR-043. The startup
  spec's section 8 sentence "`AppHost` owns the P1-P5 sequence" is corrected: the frame-loop
  `AppHost` keeps its current meaning, and the startup sequence belongs to `Application`.
- This revises the V1 non-goal of [`startup-api-v1.md`](../docs/specs/startup-api-v1.md): the
  framework manages the lifecycle, and services and developer classes observe it through
  `Application`. JSX lifecycle hooks (`useLaunchMode()`, `useAppState()`) stay deferred until the
  hook manifest exists (open question 7).

### 3. Service provisioning and resolution

Options:

| Option | How a component gets a `Navigator` | For | Against |
|---|---|---|---|
| A. Constructor injection through generated parents | each generated `Base<Self>` takes the services it and its subtree need | zero lookups; measured under 1 ns per service at mount | every ancestor's constructor changes when a leaf needs a service; codegen must compute transitive dependency sets; breaks separately generated components and designer preview |
| B. Typed `ServiceContext`, resolved once at mount | frozen registry keyed by compile-time ids; the generated base caches a typed pointer | no RTTI, no reflection; use site costs the same as A (measured); fakes are trivial in tests | about 7 to 10 ns per declared service at mount; a missing service is found at startup, not at compile time |
| B'. Same registry, lookup on every use | `context.get<Navigator>()` at each call | simplest code | adds 3 to 7 ns per call in cache and 26 to 35 ns when the component is cold (measured) |
| C. Global singletons / function-local statics | `Navigator::instance()` | no plumbing | hidden global state ([`services.md`](../docs/design/services.md) forbids it); breaks test isolation and multi-window |
| D. React-style context through the scene tree | `useContext(NavContext)` walks ancestors | familiar to React users | rejected by the component model; tree walk per lookup; needs IR support |
| E. Runtime locator keyed by `type_index` or string | `locator.get("nav")` | dynamic | needs RTTI or string keys; 20 to 66 ns per lookup, string keys allocate (measured) |
| F. Compile-time DI library (for example Boost.DI) | constructor graph generated by templates | static wiring | new dependency; long compile times; still needs a way into generated components |

**Decision: B. Services are resolved from a frozen, fixed-capacity registry exactly once per
component instance, in the generated `mount()`, and cached as typed pointers in the generated
base. No other resolution path exists for components.**

The measurements ([`service-resolve-benchmark.md`](../docs/design/service-resolve-benchmark.md),
summary in the appendix) show that a cached pointer and constructor injection cost the same at the
use site (2.1 ns for a trivial call, identical within noise), that resolving at mount costs about
60 ns for a component that needs 8 services, and that nothing allocates. Option A would save those
few nanoseconds per service at mount at the price of transitive plumbing through every generated
parent; B' and E put a lookup where it costs the most, on calls made while the frame touches many
components.

```cpp
namespace tessera::appfw {
struct ServiceId { std::uint64_t value; };               // FNV-1a of a reverse-DNS name
consteval ServiceId service_id(std::string_view name);

template <typename T>
concept ProvidedService = requires { { T::kServiceId } -> std::convertible_to<ServiceId>; };

class ServiceContext {                                   // app scope or window scope
public:
    template <ProvidedService T, std::derived_from<T> Impl>
    std::expected<void, ServiceError> provide(Impl& implementation);   // composition root only
    template <ProvidedService T> T& get() const;          // mount/start only; validated at startup
    template <ProvidedService T> T* find() const;         // optional service, may be null
};
}

// Service interface: abstract contract, the only kind of type TSX may name.
class Navigator {
public:
    static constexpr tessera::appfw::ServiceId kServiceId = tessera::appfw::service_id("tessera.nav");
    virtual void push(Route route) = 0;
    virtual void pop() = 0;
protected:
    ~Navigator() = default;
};

// Developer class (ADR-040); C++ may require interfaces or concrete provided types.
class Inbox final : public gen::InboxBase<Inbox> {
public:
    using Requires = tessera::appfw::requires_services<Navigator, MailStore>;
    void open_message(std::uint64_t id) { service<Navigator>().push(routes::message(id)); }
};
```

1. **Keys.** `ServiceId` is a `consteval` 64-bit hash of a reverse-DNS name, declared as
   `static constexpr ServiceId kServiceId` on the service interface (or on a concrete service that
   has no interface). The registry stores `{id, type tag, pointer}`; the type tag is the address
   of an `inline` variable template instantiated per type, so a lookup checks id and tag with two
   integer compares. No `typeid`, no `dynamic_cast`. A duplicate id or a tag mismatch is an error
   at `provide()`.
2. **Storage.** A fixed-capacity flat array (default 64 entries per scope, configurable), sorted
   and frozen when the application starts (measured: 1.6 us for 64 services, no allocation);
   lookup is a binary search. A dense compile-time slot index may replace the search later if
   mount profiles ever show it; the contract does not change.
3. **Scopes.** Two levels in V1: app scope and window scope; a window scope falls back to the app
   scope. Route-scoped services are open (question 2).
4. **Reaching components.** Layer 0's mount path carries one opaque `void* app_context` slot (like
   `UpdateTicket::context`), set by the window and passed by each generated parent to its children
   unchanged. The ADR-040 base reinterprets it as `const ServiceContext*` only when the component
   requires services, resolves every requirement in `mount()` before any developer code or effect
   runs, and then drops the context: `service<T>()` returns the cached reference and is a
   `static_assert` error for a `T` not in the requirement set. Components without requirements
   generate no code for this.
5. **Where resolution may happen.** Exactly three places: the composition root (`provide`,
   `freeze`, startup validation), a service's `start()` (to cache the services it depends on), and
   the generated `mount()` of a component. Never in `build`/`update`/render, event handlers,
   effects, timer callbacks, animation ticks or anything the frame pipeline calls. Enforcement: the
   generated base exposes no `ServiceContext`; `get`/`find` raise a debug assertion and a
   diagnostics counter (`appfw.service_lookup_in_frame`) when called while `RuntimeLoop` is inside a
   tick; AF1 tests require that counter to stay zero.
6. **Fail at startup, not on first click.** Codegen emits each component's requirement set (C++
   `Requires` plus TSX `useService` types) into the generated manifest; `Application` checks the
   union against the frozen scopes before P4 and reports every missing provider with the component
   and TSX location (`TESSERA_APPFW_SERVICE_MISSING`, routed to the root error boundary).
7. **Tests** build a fresh `ServiceContext`, provide fakes under the same interface ids and mount
   the component; there is no global state to reset.

#### 3.1 TSX: `useService<T>()` for service interfaces only

```tsx
export function Inbox() {
  const nav = useService<Navigator>();          // T: a registered service interface
  return <Button onClick={() => nav.push("/compose")}>Compose</Button>;
}
```

1. **Declaring interfaces.** A service interface is exported to TSX by listing it in
   `ui/services.json` (C++ header, class, id, and the methods TSX may call with their IR types),
   the same pattern ADR-028 uses for `ui/elements.json`. The compiler generates `services.d.ts`
   so editors type-check `useService` and the method calls.
2. **What the compiler accepts.** `T` must be a type argument naming an interface from the
   manifest. A concrete implementation class, a string, a variable, or an unregistered type is a
   compile error (`TESSERA_CG_SERVICE_NOT_INTERFACE`, with the TSX location). `useService` follows
   the rules of hooks: top level of the component body, fixed call order, no conditional calls.
3. **Lowering.** Each `useService<T>()` adds `T` to the component's requirement set and lowers to a
   private `T* svc_<n>_` member of the generated base, resolved in `mount()` per section 3 item 4.
   Uses of the returned value lower to calls through that pointer. There is no per-render lookup
   and no runtime string or `type_index` lookup. Calls go through the interface's `virtual`
   functions; the benchmark measures a predicted virtual call at the same cost as a non-inlined
   static call (1.8 ns), and these calls happen per user action, not per frame.
4. **What TSX may do with a service in V1.** Call manifest-listed methods from event handlers and
   effects, with arguments TSX can already express (literals, props, state, event fields). Calling
   service methods from the render body is a compile error in V1, because a plain method result is
   not reactive; reading service-owned observable state in render is open question 6. Anything
   else (implementations, registration, threads, I/O) stays in C++.
5. **Registration stays in C++.** The composition root provides implementations under the
   interface id (`app.services().provide<Navigator>(nav_impl)`); TSX cannot provide services.
6. **Preview.** The designer ([ADR-037](ADR-037-self-contained-wasm-designer.md)) gets a fake per
   manifest interface that records calls; a project may register richer preview fakes.

### 4. Service lifecycle

```cpp
class Service {                       // virtual dispatch: tens of objects, rare calls (measured, appendix)
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
6. **Dispatch.** The lifecycle base and service interfaces use `virtual` functions; this departs
   from ADR-040's CRTP style only at the service boundary. A concrete service may expose
   non-virtual, inlinable methods to C++ callers that name it directly. Any service method called
   per frame (rare; for example a value read by an animation) must be reached through a cached
   pointer to a `final` type so it can inline.

### 5. Background work: task scopes, timers, desktop and mobile

1. **`TaskScope`** (layer 1, built on the existing `Task`, `CancellationSource` and
   `CompletionQueue`): `spawn(task)`, `cancel()`, `drained()`, a bounded task count, and a
   deadline-bounded `close()` used by the shutdown sequencer. Scopes nest: app, service, window,
   component (unmount cancels). This is how "no detached tasks" becomes enforceable: the framework
   offers no way to start a task without a scope.
2. **Timers** are a layer 0 wake source: `RuntimeLoop` gains a bounded timer queue whose earliest
   deadline joins the pacer deadline, so an idle app parks until the next timer instead of polling.
   One-shot and periodic; a periodic timer coalesces missed periods; timers are deterministic under
   `ManualClock`. A timer never forces a frame; its callback schedules work like any completion.
3. **Desktop.** `Hidden` stops rendering (the `runtime-config-v1.md` section 5.8 policy, which the
   window driver must start honoring) but services keep running under their
   `BackgroundPolicy { Continue, Throttle, Pause }` (default `Continue`).
4. **Mobile.** `Hidden` asks every service to checkpoint (`on_app_state`); `Suspended` pauses all
   timers and cancels scopes whose policy is not `Continue`; in-flight I/O may finish only within
   the platform grace period, then it is cancelled. No rendering and no GPU work while not visible.
5. **OS-scheduled work** (Android WorkManager/JobScheduler, iOS BGTaskScheduler, Android
   foreground services) runs outside the visible app lifetime. The proposed contract is a
   **headless entry**: the same `Application` started without windows, with a declared subset of
   services, under the OS time budget. Scope and timing are open (question 1).

### 6. App-wide state

Options: (a) component-local state and props only (status quo); (b) context propagation through the
tree; (c) observable values owned by services; (d) a reducer/action store (Redux style); (e) a
signals graph with automatic dependency tracking.

**Decision: (a) stays the default for component state; app-wide state is (c), owned by services.**

- `Observable<T>` (layer 1) holds a value and a version on the UI thread. `subscribe(UpdateTicket&)`
  returns an RAII `Subscription`; `set()` compares with `operator==` and, on change, calls
  `schedule()` on each subscribed ticket. Updates reuse the existing `UpdateScheduler` batching:
  no synchronous re-render, one flush per tick, per-window schedulers work unchanged.
- A **store** is a service that owns observables and the methods that change them. Writes happen on
  the UI thread; background results arrive through the completion queue and then call `set()`.
- Subscriber storage is allocated at `subscribe()` (mount), never at `set()`; notifying is a loop
  over intrusive links.
- `NavController`, the async-data `DataStore` and `PermissionService` expose their state as
  observables, so every screen subscribes the same way.
- There is no `useStore` hook. In V1 a developer class subscribes in C++ and mirrors values into
  state cells with the ADR-040 setters; TSX renders those cells. Whether a manifest-listed
  observable on a service interface may be read directly in TSX render (lowered to a mount-time
  subscription) is open question 6.
- Rejected as the mandated pattern: (b) for the component-model reasons in section 3; (d) because
  reducers need switch-heavy code and an action log the V1 surface rejects (`useReducer` is unsafe
  in [`react-compat.md`](../docs/specs/jsx-intrinsics-v1/react-compat.md)), though an app may build
  one on top of (c); (e) because automatic tracking hides allocations and ordering. Derived values
  (`Computed<T>`) are deferred.

### 7. Native render surfaces and typed refs (owner decision 4)

[ADR-028](ADR-028-custom-native-elements.md) covers reusable custom elements from a manifest; this
section adds the per-instance path. Detail, example and comparison:
[`native-render-surfaces.md`](../docs/design/native-render-surfaces.md).

1. **Ref model.** TSX declares `const viewport = useRef<NativeSurface>(null)` and
   `<NativeSurface ref={viewport} composition="offscreen" redraw="onDemand" />`. `NativeSurface` is
   an intrinsic ref type; any other `T` on that element is a compile error. Codegen lowers the ref
   to a ref cell and the ADR-040 accessor `ref_<name>()`, returning a non-owning
   `NativeSurfaceRef` (node id plus generation) valid from mount to unmount and a checked no-op
   after it. TSX cannot draw; the developer class attaches a renderer in C++ (for example from a
   `useEffect(bind::attach_viewport, [])` member).
2. **One rendering contract.** `SurfaceRenderer` reuses the ADR-028 hooks: attach, resize (pixels
   and DPI scale), visibility, render (RHI `CommandList`, target, damage rect), input (normalized,
   surface-local, through capture/target/bubble), device lost/restored, detach. At most one call
   per frame per surface, so `virtual` is acceptable (appendix).
3. **Ownership and lifetime.** The scene owns the node, the developer class or a service owns the
   renderer, and generated `unmount()` detaches before the developer class is destroyed. GPU
   resources retire through the frames-in-flight queue; device loss drops and later recreates them;
   nothing waits on a fence on the UI thread.
4. **Threading.** V1 renders on the UI thread in the frame pipeline's record step. Worker
   rendering with texture handoff (a ring of at least three targets, completion published through
   the `CompletionQueue`, composited without waiting) follows once the ADR-049 RHI threading rules
   allow it (open question 9).
5. **Dirty rendering and static idle.** A surface redraws only on `request_redraw()`, resize,
   visibility change or device restore, or each frame with `redraw="continuous"` while visible and
   `Active` (a pacer wake that stops on `Hidden`, `Occluded`, `Suspended`). Its rectangle feeds
   damage and scissoring like any node; a static UI with an idle surface presents nothing.
6. **Composition.** `offscreen` (default) is composited by the scene graph as a textured quad, so
   transforms, clips, opacity and z-order apply; ADR-028 `inline` draws into the window target;
   a direct OS sub-surface is deferred (open question 9).
7. **Backend-agnostic seam.** Renderers use only public RHI types and precompiled shaders, so one
   renderer runs on Vulkan, WebGPU (Wasm) and the null backend (structural tests; goldens from
   lavapipe or software). Raw handle interop is a per-backend extension under ADR-049, not V1.

### 8. Module placement (each module keeps or gets its own spec)

| Module | In-tree scope | Community scope |
|---|---|---|
| `storage` (template default) | settings key/value with versioned schema, atomic document writes, cache directory, migrations, keystore interface + Windows/Linux implementations | database adapters, cloud sync (SQLite placement is open question 4) |
| `navigation`, `restore`, `data`, `i18n` (template default); `shell` | as specified by [ADR-030](ADR-030-app-platform-completeness.md) and its specs, now as modules | extra platform implementations |
| `permissions` (new spec) | state model (`NotDetermined`, `Granted`, `Denied`, `DeniedPermanently`) and just-in-time request flow | per-platform permission back ends beyond the in-tree platforms |
| `notifications` | local notifications, activation routing | push providers (FCM, APNs, Windows push) |
| `net` | HTTP client interface, cancellation/timeout/retry rules, deterministic fake | transport back ends (open question 5) |
| `updates` | the signed-manifest reference flow of [`updates-and-distribution-v1.md`](../docs/specs/updates-and-distribution-v1.md) | store-specific and MDM updaters |

### 9. In-tree versus community

The tiers of ADR-049 apply unchanged to app-framework providers: Tier 1 in-tree and release
blocking (`appfw_core`, every module interface, its fake, and the Windows/Linux implementations);
Tier 2 community-maintained with a named maintainer and a passing contract suite; Tier 3
experimental, out of tree or behind an OFF option. Each module interface ships a **contract test
suite** run against its fake, which a provider links and runs. Contributed providers follow
ADR-049's licensing rules: MIT code, static linking, no GPL/LGPL, entries in
`docs/specs/dependencies.md` and `THIRD_PARTY_NOTICES.md`. The **contracts** (lifecycle states,
`ServiceContext`, `Service`, service interfaces, `TaskScope`, `Observable`, `SurfaceRenderer`,
module interfaces) stay core-owned and change only by ADR or spec.

### 10. Hard-constraint check

| Constraint | How this design meets it |
|---|---|
| No RTTI or reflection | compile-time ids plus per-type address tags; requirements are C++ types checked by `static_assert`; TSX `useService` types are checked against a manifest at compile time |
| No hidden allocations on hot paths | fixed-capacity registries frozen at start; resolution only at mount (0 allocations measured); subscriptions allocate at mount only; surface targets created on attach/resize |
| Never block the UI thread | async `start`/`stop`; shutdown and termination are tick-driven; surface texture handoff checks completion without waiting |
| Static UI idles | timers and continuous surfaces join the pacer deadline; on-demand surfaces redraw only on request |
| No detached tasks | tasks start only inside a `TaskScope`; scopes close under a deadline |
| Exceptions only at boundaries | the service and renderer boundaries catch and convert to `Failed` plus a diagnostic |
| Static linking, MIT | every layer is a static library; providers follow ADR-049 section 5 |

## Consequences

Positive:

- Apps get one programming model, as on Flutter or React Native: lifecycle, services, background
  work, state, storage and navigation are managed by the framework and present in every new app.
- One answer to "how does a component reach X": C++ `Requires` or TSX `useService<Interface>()`,
  checked at compile time and validated against providers at startup, with a use-site cost equal
  to constructor injection (measured).
- Native 3D and custom drawing get a typed, lifecycle-safe handle that respects dirty rendering,
  static idle and backend independence.
- Lifecycle, background and shutdown behavior are testable headlessly: scripted events,
  `ManualClock`, `ManualExecutor`.
- The layer split still lets layer 0 be tested, embedded or ported alone.

Negative and risks:

- A large layer to maintain, and [`startup-api-v1.md`](../docs/specs/startup-api-v1.md),
  [`services.md`](../docs/design/services.md), module-map and component-model texts must be revised.
- Default-linked modules raise the floor of every app's binary and build time; mitigated by
  section garbage collection and per-app opt-out in `tessera.json`.
- Missing providers are found at startup, not at compile time: the composition root is ordinary C++
  and cannot be checked against every component's needs statically without making the app type a
  template parameter everywhere.
- Two manifests (`services.json`, `elements.json`) and generated `.d.ts` files to keep in sync with
  C++ headers; drift is a build error, not a silent mismatch.
- The opaque `app_context` pointer is untyped inside layer 0; misuse is prevented only by the
  generated base being the sole reader.
- Service provisioning, `useService` and ref codegen depend on ADR-040's `Base<Self>` codegen,
  which is not implemented yet.
- `virtual` service interfaces depart from ADR-040's CRTP-only style; the cost is measured as
  negligible for per-action calls but blocks inlining, so per-frame calls must use `final` types.

## Alternatives considered

- **Stay a UI toolkit; leave app structure to each app.** Rejected by owner decision 1.
- **Put services in the core runtime** (what the current module map implies). Rejected: inverts
  the layering and makes layer 0 untestable without the app pieces.
- **Qt-style meta-object system** (signals, properties, runtime lookup by name). Rejected: needs
  reflection or a moc-like generator for every C++ type, which the RTTI rule excludes.
- **Constructor injection through generated parents** (A), **lookup per use** (B'),
  **singletons** (C), **React context** (D), **runtime locators** (E) and **a DI library** (F):
  see section 3 and the measurements.
- **`useService` with concrete types or string keys.** Rejected by owner decision 2: TSX names
  contracts only, implementations stay in C++.
- **`useStore` / Redux-style store as the only state model** and **signals with automatic
  tracking**: see section 6.
- **Native surface through a raw platform child window** (HWND/NSView). Rejected for V1: breaks
  clipping, transforms and z-order inside the scene, and is not portable to Wasm.

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

Still open:

1. **OS-scheduled background work** (WorkManager, BGTaskScheduler, foreground services): V1 or
   later? On desktop, is a tray-resident app (`quitOnLastWindowClosed = false`) in scope?
2. **Route-scoped services**: app and window scopes only in V1, or route scopes too?
3. **Multi-window**: one `Application` with several `WindowHost`s in V1? (The startup spec defers
   several windows per App subtree; `app-host.md` expects one host per window.)
4. **Database**: in-tree SQLite adapter (public domain, statically linkable) or community only?
5. **Networking stack**: platform HTTP stacks (WinHTTP, NSURLSession, Android) versus a vendored
   one; this decides whether `net` back ends are in-tree.
6. **State model**: are plain observables enough, or does devtools time-travel
   ([`devtools-protocol-v1.md`](../docs/specs/devtools-protocol-v1.md)) require an action log
   from the start? May TSX read manifest-listed service observables in render (lowered to a mount
   subscription)?
7. **Startup spec revision**: agree that `Application` (not the frame-loop `AppHost`) owns P1-P5,
   that the C++ lifecycle API is part of V1, and when JSX lifecycle hooks arrive?
8. **Default module set**: is `storage`, `navigation`, `restore`, `data`, `i18n` the right default
   for the app template?
9. **Native surfaces**: is worker rendering with texture handoff needed in V1, and is a direct OS
   sub-surface (overlay plane, child swapchain) in scope?

## Follow-up (if accepted)

1. Fix the dependency direction in [`module-map.md`](../docs/architecture/module-map.md) section 2
   and the layer table in [`overview.md`](../docs/architecture/overview.md) section 5.
2. Rewrite [`services.md`](../docs/design/services.md) sections 1, 7 and 9 around sections 3-5 of
   this ADR; add `docs/design/app-framework.md` (lifecycle, scopes, provisioning, stores, surfaces).
3. Revise [`startup-api-v1.md`](../docs/specs/startup-api-v1.md) sections 1, 8 and 10 (owner of
   P1-P5, C++ lifecycle API) and [`component-model-v1.md`](../docs/specs/component-model-v1.md)
   section 12 (dependency injection resolved by this ADR); add `useService` and
   `useRef<NativeSurface>` to the hook manifest and the JSX subset spec.
4. Add `AppForeground`/`AppBackground` to [`os-events-v1.md`](../docs/specs/os-events-v1.md) and
   the timer wake source and shutdown budget fields to
   [`runtime-config-v1.md`](../docs/specs/runtime-config-v1.md).
5. New specs: `services-manifest-v1.md` (`ui/services.json`), `native-surface-v1.md`,
   `permissions-v1.md`, `storage-v1.md`, `notifications-v1.md`, and later the headless background
   entry. Amend ADR-028's follow-up to share the `SurfaceRenderer` contract.
6. Fix the stale texts listed in section 3 of the audit (protected regions in component-model and
   overview, I/O executor status).

## Appendix: service-resolve measurements

Summary of [`service-resolve-benchmark.md`](../docs/design/service-resolve-benchmark.md) (AMD Ryzen 5
4600H, Windows 11, MSVC 19.51 x64 Release, median of 9 repetitions, two runs agree within about 5%
except where noted there). Microbenchmark limits (single machine and compiler, per-iteration
compiler barrier, perfectly predicted branches, trivial method) are listed in that document.

| Measurement | Result | allocs |
|---|---|---:|
| Use site, cached pointer from mount (B) | 2.1 ns per call | 0 |
| Use site, constructor-injected reference (A) | 2.1 ns per call | 0 |
| Lookup every call, frozen registry of 8 / 64 (B') | 5.3 / 8.0 ns per call | 0 |
| Lookup every call, 1M components out of cache (B' vs B) | 100 vs 68 ns per call | 0 |
| `unordered_map<type_index>` per call, 8 / 64 services (E) | 20 / 40 ns per call | 0 |
| `unordered_map<string>` per call (E) | 66 ns per call | 1 |
| Mount, registry lookup, N = 1 / 8 / 64 services | 7 / 60 / 650 ns per component | 0 |
| Mount, injected pointer copy, N = 1 / 8 / 64 | 0.8 / 3 to 8 / 10 to 66 ns per component | 0 |
| Provide 64 services and freeze | 1.6 us once | 0 |
| Trivial service call: inlined static / CRTP / non-inlined static / virtual | 2.1 / 2.1 / 1.8 / 1.8 ns | 0 |
