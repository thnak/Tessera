# App-framework gap audit v1

Status: analysis (docs-only, 2026-11-12). Decision proposed in
[ADR-050](../../decisions/ADR-050-opt-in-app-framework-layer.md) (application-framework layer,
revised with owner decisions). This document records what Tessera specifies and implements for
the concerns that C++ UI toolkits such as Qt and JUCE treat as *application framework*
(lifecycle, services, threading, dependency provisioning, app state, storage, navigation, and so
on), where the documents contradict each other or the code, and how other toolkits draw the line. It does
not change any spec.

Terms: **specified** = a normative spec or design note defines the behavior; **implemented** = code
and tests exist on `main`; **missing** = neither a contract nor code exists; **contradictory** = two
documents, or a document and the code, disagree. A row can carry more than one status. Big Steps
(B2, F, ...) are defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step).

## 1. Why this audit

[`overview.md`](overview.md) section 1 calls Tessera "an **application framework**", but the
documents were written from the renderer outwards: the frame loop, style, layout, input and the
async primitives are real; the app-level pieces are a set of separate specs
([ADR-030](../../decisions/ADR-030-app-platform-completeness.md),
[ADR-031](../../decisions/ADR-031-production-tooling-and-distribution.md)) with no shared model for
how an app starts its services, how components reach them, what happens on background/foreground,
or where app-wide state lives. The gap register in
[`app-completeness-cross-check-v1.md`](app-completeness-cross-check-v1.md) covers feature breadth
(text, scroll, overlays, forms, ...); it does not cover these structural questions.

## 2. Capability matrix

| Capability | Status | Evidence |
|---|---|---|
| App lifecycle, desktop | specified; partially implemented; contradictory | `WindowHost::run()` loops until the window closes ([`window.hpp`](../../include/tessera/app/window.hpp)); the driver handles only `Close`, `Resize`, `DpiChange` and `SystemSettingsChanged` and drops `Focus`, `Blur`, `Occluded`, `Minimized`, `Restored` (`src/app/window.cpp`, event switch in `run()`), so the occluded/unfocused policy of [`runtime-config-v1.md`](../specs/runtime-config-v1.md) section 5.8 is not applied. `Close` ends the loop with no veto hook (unsaved-work guard of [`navigation-v1.md`](../specs/navigation-v1.md) section 10.1). There is no application object: the composition root is a hand-written `main()` ([`samples/click-demo/main.cpp`](../../samples/click-demo/main.cpp)). Startup phases P0-P5 and launch modes are specified ([`startup-api-v1.md`](../specs/startup-api-v1.md) section 5, ADR-042, ADR-043), but the same spec rules out "a general app-lifecycle API in V1" (sections 1 and 10) |
| App lifecycle, mobile background/foreground | specified (platform side only); missing (app side) | `Suspend`, `Resume`, `ScreenOff`, `MemoryPressure`, `ShutdownRequested` in [`os-events-v1.md`](../specs/os-events-v1.md) section 2, Android mapping section 7, runtime mapping section 11; surface rules in [`mobile-integration.md`](../design/mobile-integration.md) sections 3.1 and 3.5. No normalized app-level `Foreground`/`Background` distinct from window visibility; no contract for how services or app code observe these events. "Background execution policy on mobile" is open in [`services.md`](../design/services.md) section 10 and [`mobile-integration.md`](../design/mobile-integration.md) section 7. `src/platform/android/` is empty |
| Background services and tasks with a lifecycle | missing | [`services.md`](../design/services.md) section 1 defines only `class Service { virtual void shutdown() = 0; }`; section 7 lists intentions (cancel or checkpoint on suspend). No states, start order, dependency order, failure policy, or shutdown deadline. Startup P3 says "services, session/auth ... run off the UI thread" ([`startup-api-v1.md`](../specs/startup-api-v1.md) section 5.2) without defining what a service is. OS-scheduled work (Android WorkManager, iOS BGTaskScheduler) is not mentioned anywhere. `src/services/*` hold only `.gitkeep` |
| Threading and executors | implemented (core); specified (rest) | Implemented: lazy `Task<T>` resumed only through `CompletionQueue` ([`task.hpp`](../../include/tessera/runtime/task.hpp)), `CancellationSource`/`Token`, bounded `CompletionQueue` with priorities and `ManualExecutor` ([`async.hpp`](../../include/tessera/runtime/async.hpp)), `WorkerExecutor`, `IoExecutor`, `RuntimeLoop` draining completions ([`runtime_loop.hpp`](../../include/tessera/runtime/runtime_loop.hpp)). Missing: a structured owner for a group of tasks (task scope), timers/deadlines as wake sources (`RuntimeLoop` has no timer API), `when_all`/`when_any`/`timeout` ([`async.md`](../design/async.md) section 2.1 limitations). Rules are consistent: no detached tasks, no UI-thread waits ([`threading-and-memory.md`](../design/threading-and-memory.md)) |
| Service provisioning (dependency injection) | missing; contradictory | [`component-model-v1.md`](../specs/component-model-v1.md) section 1 and 8 reject context; section 12 lists "context-free dependency injection for services" as open; [`react-compat.md`](../specs/jsx-intrinsics-v1/react-compat.md) marks `useContext` unsafe. Meanwhile [`navigation-v1.md`](../specs/navigation-v1.md) section 10.1 (`NavController`), [`async-data-v1.md`](../specs/async-data-v1.md) section 8.1 (`DataStore`), [`platform-shell-v1.md`](../specs/platform-shell-v1.md) section 9.1 (`ShellService`) and [`state-restoration-v1.md`](../specs/state-restoration-v1.md) section 7.1 (`RestoreService`) assume components reach those objects. [ADR-040](../../decisions/ADR-040-generated-base-and-developer-class.md) gives developer code a home (a class derived from `gen::<Component>Base<Self>`) but no way to receive dependencies; its codegen is not implemented yet (no `Base<Self>` in `src/compiler/codegen/`) |
| App-wide state / store | missing | Component-local `useState` routed through `UpdateTicket`/`UpdateScheduler` ([`update_scheduler.hpp`](../../include/tessera/ui/scene/update_scheduler.hpp)); local stores exist (`ItemStateStore` in [`item_state_store.hpp`](../../include/tessera/ui/virtual/item_state_store.hpp), the form model in `include/tessera/ui/forms/`). No cross-screen observable state; `useReducer`/`useContext` rejected ([`react-compat.md`](../specs/jsx-intrinsics-v1/react-compat.md) section 4). `DataStore` is specified for typed server data only and not implemented |
| Persistence, cache, database | specified (partly); missing (code) | Storage classes (settings, documents, cache), atomic writes and migrations are bullets in [`services.md`](../design/services.md) section 4; there is no storage spec. Restore snapshot format is specified ([`state-restoration-v1.md`](../specs/state-restoration-v1.md) section 7.1); data disk cache in [`async-data-v1.md`](../specs/async-data-v1.md) section 2; asset cache in [`resource-lifecycle-v1.md`](../specs/resource-lifecycle-v1.md). No embedded database anywhere; keystore abstraction is an open question ([`security.md`](../design/security.md) section 6). `src/services/storage/` and `src/hal/filesystem/` are empty |
| Navigation | specified; not implemented | Route table, stack, guards, epochs, back order ([`navigation-v1.md`](../specs/navigation-v1.md) section 10.1), deep links ([`deep-linking-v1.md`](../specs/deep-linking-v1.md)). Owner per window is stated; how a screen's handler obtains its `NavController` is not. `src/services/navigation/` is empty |
| Configuration | specified; partially implemented | [`project-config-v1.md`](../specs/project-config-v1.md) and [`runtime-config-v1.md`](../specs/runtime-config-v1.md) section 6 (`RuntimeConfig` mirror). Code has per-module structs (`HostConfig`, `RuntimeLoopConfig`, `FramePacerConfig`) and a manifest parser ([`automation/app_config.hpp`](../../include/tessera/automation/app_config.hpp)); no `AppConfig`/`RuntimeConfig` object and no app-defined (user) settings schema |
| Logging and diagnostics | implemented (core); specified (rest) | Bounded `Logger` with sinks and redaction ([`log.hpp`](../../include/tessera/diagnostics/log.hpp)), metrics registry, frame counters ([`diagnostics.hpp`](../../include/tessera/runtime/diagnostics.hpp)); crash/trace specified ([`crash-reporting-v1.md`](../specs/crash-reporting-v1.md), [`diagnostics-config-v1.md`](../specs/diagnostics-config-v1.md)). How app code obtains *the* logger (global or injected) is undecided |
| IPC and bridges | specified; partially implemented | Policy: [`bridges.md`](../design/bridges.md), [ADR-024](../../decisions/ADR-024-native-first-boundaries.md). Implemented: harness transports (`src/harness/`), UIA and AT-SPI accessibility bridges. Single-instance forwarding of activations ([`deep-linking-v1.md`](../specs/deep-linking-v1.md) section 6) and the devtools protocol ([`devtools-protocol-v1.md`](../specs/devtools-protocol-v1.md)) are not implemented. No app-to-app or app-to-helper-process IPC contract (only design-time IPC) |
| Permissions | missing (scattered) | Mentioned in [`services.md`](../design/services.md) sections 6 and 8, [`platform-shell-v1.md`](../specs/platform-shell-v1.md) section 9.1 (`ShellCapabilities::permissioned`), [`mobile-integration.md`](../design/mobile-integration.md) section 3.7, [`security.md`](../design/security.md). No owner, state model (not determined / granted / denied / permanently denied) or API |
| Notifications | specified (design only); missing (code) | [`services.md`](../design/services.md) section 6; notification activation in [`startup-api-v1.md`](../specs/startup-api-v1.md) section 5.1 and [`deep-linking-v1.md`](../specs/deep-linking-v1.md). No spec file; push providers (FCM/APNs) unmentioned. `src/services/notification/` is empty |
| Updates | specified; not implemented | [`updates-and-distribution-v1.md`](../specs/updates-and-distribution-v1.md) section 6.1 (signed manifest, staging, health check, rollback) |
| Testing seams | implemented (UI/runtime); missing (app level) | `ManualClock`, `ManualExecutor`, pumped `CompletionQueue`, headless `AppHost` (`tests/unit/app_host_tests.cpp`), `WindowHost::post_input`/`apply_metrics`, `capture_frame`, the automation harness. Missing: service fakes/overrides, lifecycle event injection at app level, fake file system/network/keystore |

## 3. Contradictions and stale text

1. **Dependency direction.** [`module-map.md`](module-map.md) section 2 draws
   `runtime ── ui + rendering + ... + services + data`, so the runtime would link the services; the
   layer table in [`overview.md`](overview.md) section 5 says `services` may depend only on `core`
   and `hal`. Neither puts app-level services *above* the runtime, which is where they need to be
   to use executors, the completion queue and lifecycle events.
2. **Two meanings of `AppHost`.** [`startup-api-v1.md`](../specs/startup-api-v1.md) section 8 says
   `AppHost` owns the P1-P5 sequence, loading factories and a `Stage`; the implemented `AppHost`
   ([`app-host.md`](../design/app-host.md)) is the per-window frame loop and owns none of that.
3. **Lifecycle API.** [`startup-api-v1.md`](../specs/startup-api-v1.md) sections 1 and 10 exclude a
   lifecycle API from V1, while [`services.md`](../design/services.md) section 7 requires services
   to react to suspend/resume and [`startup-api-v1.md`](../specs/startup-api-v1.md) section 5.1
   requires hot resume to revalidate session and data in the background.
4. **Context.** Rejected by the component model, but four specs (row "Service provisioning")
   presuppose a way to reach app-scoped objects.
5. **Synchronous `Service::shutdown()`** in [`services.md`](../design/services.md) section 1
   conflicts with [`async.md`](../design/async.md) section 5 ("shutdown cancels scopes, drains
   bounded queues, and waits with a timeout") and the no-blocking rule.
6. **Protected regions still described as current** in
   [`component-model-v1.md`](../specs/component-model-v1.md) sections 4 and 9 and
   [`overview.md`](overview.md) section 3, although ADR-040 superseded ADR-008.
7. **I/O executor status.** [`async.md`](../design/async.md) says "an I/O executor is not implemented
   yet" in its status paragraph but documents the implemented `IoExecutor` in section 2.1; the
   status table in [`docs/README.md`](../README.md) still says "I/O executor pending".
8. **Services order is stale** (already finding 10 of the cross-check, not fixed):
   [`services.md`](../design/services.md) section 9 lacks shell, restore and data.

## 4. How other toolkits draw the line

| Concern | Qt | JUCE | Flutter | Jetpack Compose (Android) |
|---|---|---|---|---|
| App object and lifecycle | `QGuiApplication`, `applicationStateChanged`, `aboutToQuit`, `quitOnLastWindowClosed` | `JUCEApplication::initialise/shutdown/suspended/resumed/systemRequestedQuit` | `AppLifecycleListener` (resumed, inactive, hidden, paused, detached) | `Lifecycle`/`LifecycleOwner`, `ProcessLifecycleOwner` |
| Background work | `QThreadPool`, `QtConcurrent`, `QFuture`; no mobile scheduler | `ThreadPool`, `Timer`, `MessageManager::callAsync` | isolates, `compute`; WorkManager via community plugins | coroutines, `viewModelScope`; WorkManager |
| Dependency provisioning | QML context properties and singletons; built on the meta-object system (moc) | `SharedResourcePointer` singletons | `InheritedWidget`; Provider and Riverpod are community packages | Hilt/Dagger (compile-time, annotation processing); `CompositionLocal` |
| App-wide state | `QObject` properties and signals | `ValueTree` (observable tree with undo) | `ChangeNotifier`, `ValueNotifier`; Riverpod/Bloc community | `ViewModel`, `StateFlow`, `SavedStateHandle` |
| Storage | `QSettings`, `QStandardPaths`, Qt SQL | `PropertiesFile`, `ApplicationProperties` | `shared_preferences`, `sqflite` (plugins) | DataStore, Room (separate libraries) |
| Navigation | QML `StackView` | none | `Navigator`, `go_router` (community) | Navigation-Compose (separate library) |
| IPC | `QLocalServer`/`QLocalSocket`, D-Bus | `InterprocessConnection`, `ChildProcess` | platform channels | Binder/Intents (platform) |
| Packaging of these pieces | separate modules (QtCore, QtGui, QtNetwork, QtSql), each separately linkable | modules selected per project | small core; most app concerns live in packages outside the SDK | separate, separately versioned Jetpack libraries |

Observations that matter for Tessera:

- Every toolkit has an app object and a lifecycle callback set; it is the one piece nobody leaves
  to the community.
- The toolkits that keep the UI core small (Flutter, Compose) still ship lifecycle, a task scope
  and a dependency mechanism with the platform, and push storage, navigation and state libraries
  into separately linkable packages.
- Qt and Hilt rely on code generation or a meta-object system for provisioning. Tessera already has
  a code generator ([ADR-040](../../decisions/ADR-040-generated-base-and-developer-class.md)) and
  must not use RTTI or reflection on hot paths, so compile-time ids are the natural fit.

## 5. What this audit does not cover

Feature breadth already tracked in the cross-check (text, scroll, overlays, forms, i18n,
accessibility), the GPU backend surface ([ADR-049](https://github.com/thnak/Tessera/pull/83),
backend extensibility, open as a pull request), and the designer. The proposed direction and
roadmap are in [ADR-050](../../decisions/ADR-050-opt-in-app-framework-layer.md).
