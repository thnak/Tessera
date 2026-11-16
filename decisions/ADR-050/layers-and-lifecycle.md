# Layers and the application lifecycle

Part of the [ADR-050 decision set](README.md). Section bodies here: `## Decision`, `### 1.` and `### 2.`. The heading
numbers, levels and order are the original ones, so `ADR-050 section N` still names the same
heading. Each part is a verbatim slice of the former single file; the shared front matter
(status, date, related decisions, terms) is in the [index](README.md).

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
| `Launching` | P0-P3 of [`startup-api-v1.md`](../../docs/specs/startup-api-v1.md) | process start | `onCreate` | `scene(_:willConnectTo:)` |
| `Active` | visible and focused | a window focused | `onResume` / top-resumed | `sceneDidBecomeActive` |
| `Inactive` | visible, not focused | all windows blurred | `onPause` (multi-window) | `sceneWillResignActive` |
| `Hidden` | no visible window; process runs | all windows minimized/occluded/closed with `quitOnLastWindowClosed = false` | `onStop` | `sceneDidEnterBackground` |
| `Suspended` | process will be frozen or may be killed | OS sleep (`Suspend`) | process cached after `onStop` + `onTrimMemory` | background grace expired |
| `Terminating` | shutdown sequence running (section 4) | last window closed or `ShutdownRequested` | `onDestroy` of the last activity / finish | `applicationWillTerminate` |

- [`os-events-v1.md`](../../docs/specs/os-events-v1.md) gains `AppForeground`/`AppBackground`
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
- This revises the V1 non-goal of [`startup-api-v1.md`](../../docs/specs/startup-api-v1.md): the
  framework manages the lifecycle, and services and developer classes observe it through
  `Application`. JSX lifecycle hooks (`useLaunchMode()`, `useAppState()`) stay deferred until the
  hook manifest exists (open question 7).
