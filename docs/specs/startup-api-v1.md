# Startup API v1: app startup surface, phases, and the framework loading screen

Status: proposed (Big Step C for the JSX surface, Big Step B2 for the runtime, Big Step F for the
platform launch artifacts; all defined in [AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step)).
Decision records: [ADR-042](../../decisions/ADR-042-jsx-startup-surface.md) (member tags, `App` root,
declaration hoisting) and [ADR-043](../../decisions/ADR-043-launch-modes-and-state-reuse.md) (launch
modes, reuse matrix, activation precedence). Companions: [startup-and-loading.md](../design/startup-and-loading.md) (UX
rationale and loading modes), [resource-lifecycle-v1.md](resource-lifecycle-v1.md) (load/lease/
unload mechanics), [jsx-subset-v1.md](jsx-subset-v1.md) (accepted syntax),
[project-config-v1.md](project-config-v1.md) (`loading` block), [error-boundaries-v1.md](error-boundaries-v1.md)
(root fallback), [state-restoration-v1.md](state-restoration-v1.md), [navigation-v1.md](navigation-v1.md).

This document defines the developer-facing startup contract: what an app declares in TSX, what the
framework guarantees at each startup phase, and which generated artifacts and diagnostics follow.
It does not redefine resource mechanics, restore order, or routing; those stay normative in the
documents linked above.

## 1. Purpose and non-goals

Purpose: make startup a declarative, compile-time-checked part of the app source, so a developer
gets a good first frame and a replaceable loading screen without writing phase code, timers,
preload calls, or platform bootstrap code.

Non-goals:

- A JavaScript runtime, suspense, `lazy`, or code splitting ([ADR-024](../../decisions/ADR-024-native-first-boundaries.md),
  [component-model-v1.md](component-model-v1.md) section 8).
- Dynamic startup configuration: the startup declaration is compile-time metadata; runtime policy
  changes go through `tessera.json` or the programmatic `AppConfig` (section 7).
- A new resource model: `Resources.*` hints feed the existing lifecycle
  ([resource-lifecycle-v1.md](resource-lifecycle-v1.md)).
- Per-phase developer callbacks or a general app-lifecycle API in V1 (section 10).

## 2. Terms

| Term | Meaning |
|---|---|
| Startup declaration | The `<App>` subtree of the app entry component: `Startup.Loading`, `Startup.Resources`, `Resources.*` and the UI children |
| App entry | The component named by `app.entry` ([project-config-v1.md](project-config-v1.md)); its returned element is the startup declaration or a plain UI tree |
| Implicit App | The framework-synthesized `<App>` used when the entry component returns a plain UI tree (section 4.1) |
| Loading subtree | The single element child of `Startup.Loading`, or the framework default loading screen when absent |
| Critical set | Resources with `priority="critical"` plus `loading.criticalResources` (section 7); the only resources that may delay the app UI |
| First frame (P2) | The first presented frame: the loading subtree, never app UI |
| First meaningful frame | The first presented frame of the app UI tree (P4), matching the definition in [loading-matrix.md](../guides/loading-matrix.md) |
| Interactive (P4) | Input is dispatched to the app UI tree and the first inspection of its layout has completed |
| Phase | One of P0-P5 (section 5.2); `stage` in diagnostics and metrics |
| Launch mode | `cold`, `warm`, `hot_resume` or `process_restore` (section 5.1); framework-determined, never declared by the app |
| Activation | An external target that requested the launch: `none`, `deep_link` or `notification` (section 5.1) |
| Default loading screen | The framework-owned loading subtree compiled into every app (section 6) |

## 3. Developer model

```tsx
// ui/App.tsx — app.entry
import { App, Startup, Resources } from "tessera";
import { HomePage } from "./HomePage";
import { AppLoading } from "./AppLoading";

export function MyApp() {
  return (
    <App restoreState>
      <Startup.Loading>
        <AppLoading />
      </Startup.Loading>

      <Startup.Resources>
        <Resources.Font name="app-font" src="assets/fonts/Inter-Regular.ttf" priority="critical" />
        <Resources.Image name="home-logo" src="assets/images/logo.png" priority="high" />
      </Startup.Resources>

      <HomePage />
    </App>
  );
}
```

Rules:

1. `<App>` is the root startup element. It must be the element returned by the app entry component
   (section 4.1), directly or through a fragment-free tree.
2. Children of `<App>` are either declaration elements (`Startup.Loading`, `Startup.Resources`) or
   the initial UI tree. Declaration elements are hoisted into the startup declaration and produce
   no scene nodes (section 4.4).
3. The initial UI tree is what P4 mounts; there is no `initialScreen` prop. A screen that needs a
   route table composes with navigation inside the children
   ([navigation-v1.md](navigation-v1.md)).
4. If the entry component returns a plain UI tree (no `<App>`), codegen synthesizes an Implicit App
   with the default loading screen and no resource hints, and reports
   `TESSERA_JSX_STARTUP_APP_IMPLICIT` (warning). This keeps existing projects running.
5. `App`, `Startup` and `Resources` are reserved built-in names (section 4.4, rule 12); a project
   component or local declaration with one of those names is an error.
6. The launch mode and activation are environment-determined (section 5.1). The declaration selects
   neither; it only declares structure, resource hints, the loading subtree and `restoreState`.

## 4. Element contract

The startup elements are framework elements declared in this spec. They are not rows of the
13-intrinsic table ([jsx-intrinsics-v1](../specs/jsx-intrinsics-v1/README.md)); they lower to the
startup declaration, not to scene nodes.

### 4.1 `<App>`

| Prop | Type | Req | Default | Rule |
|---|---|---|---|---|
| `restoreState` | boolean flag | no | `restore.enabled` from [project-config-v1.md](project-config-v1.md) | Overrides the config master switch; JSX wins (section 7.3) |

- Exactly one `<App>` per component; it must be the returned element of the app entry component.
  Any other placement is `TESSERA_JSX_STARTUP_APP_PLACEMENT`.
- `App` must receive at least one non-declaration child (the initial UI tree); zero is
  `TESSERA_JSX_STARTUP_APP_EMPTY`.
- `App` accepts no `className`, `style`, event props, `key`, or `ref`; unknown props are
  `TESSERA_JSX_SUBSET_UNKNOWN_PROP` as elsewhere.
- The App root is not a scene node: the initial UI tree is mounted directly under the scene root, so
  the first UI child controls the root layout box.

### 4.2 `<Startup.Loading>`

| Child | Req | Rule |
|---|---|---|
| exactly one element | no | The loading subtree; absent means the framework default loading screen |

- At most one per `<App>`; a second is `TESSERA_JSX_STARTUP_DUPLICATE`.
- The child is one element (a project or library component tag), not text, a fragment, a
  conditional, or a loop; otherwise `TESSERA_JSX_STARTUP_LOADING_CHILD`.
- The loading subtree must be renderable at P2, i.e. it may use only:
  - built-in geometry, `Path`, theme tokens, and style properties;
  - images and fonts declared `priority="critical"` in the same `<App>`.
  An `<Image>` in the loading subtree whose resource is not `critical` is
  `TESSERA_JSX_STARTUP_LOADING_RESOURCE` (error). Fonts cannot be resolved statically; the
  requirement is normative on the developer and is reported at run time
  (`TESSERA_STARTUP_LOADING_FONT`).
- The loading subtree must have an accessible name and expose progress semantics (section 6.3);
  a custom subtree that provides neither gets the framework wrapper described in section 6.3.
- It must not trap focus, start timers, or block input.

### 4.3 `<Startup.Resources>` and `Resources.*`

`Startup.Resources` is a declaration container: it accepts only `Resources.*` children, at most one
per `<App>` (`TESSERA_JSX_STARTUP_DUPLICATE` otherwise), and may be empty.

| Element | Props | Rule |
|---|---|---|
| `Resources.Font` | `name` string, `src` string literal, `priority` enum | `src` must be listed by `assets.fonts` (`TESSERA_JSX_STARTUP_ASSET`); `priority` defaults to `high` |
| `Resources.Image` | `name` string, `src` string literal, `priority` enum | `src` must be inside a directory or file of `assets.images`, PNG/JPEG/WebP within [resources.md](../design/resources.md) limits (`TESSERA_JSX_STARTUP_ASSET`) |
| `Resources.Data` | `name` string, `src` string literal, `priority` enum | Opaque byte payload (`ResourceClass::Data`); not a query cache ([async-data-v1.md](async-data-v1.md)) |

- `priority`: `"critical" | "high" | "low"`. Defaults: `high`. Meaning:
  - `critical` — part of the critical set; the app UI (P4) waits for it.
  - `high` — starts in P3; does not delay P4; may still be loading when the app UI mounts.
  - `low` — prefetch only, in P5 idle windows; cancellable and dropped under power-save.
  Priorities lower to the existing integer `RequestOptions.priority` (higher = sooner) with the
  total order `critical > high > low` ([resource-lifecycle-v1.md](resource-lifecycle-v1.md)).
- `name` is the runtime resource id used by metrics, preload lists and config cross-references. It
  is required in V1 so that `loading.criticalResources` and diagnostics have a stable id; a
  duplicate id inside one project is `TESSERA_JSX_STARTUP_RESOURCE_ID`.
- `src` is a string literal, never an expression or URL; assets are build-time metadata and ids are
  runtime metadata ([resources.md](../design/resources.md)).
- Declaration order is the load order inside a priority class and is deterministic; the golden mode
  pins it ([resource-lifecycle-v1.md](resource-lifecycle-v1.md) section on determinism).

### 4.4 Accepted and rejected forms

Accepted, and only these:

1. `<App>` as the returned root of the app entry component, with declaration children and UI
   children.
2. `<Startup.Loading>` with exactly one element child, only as a direct child of `<App>`.
3. `<Startup.Resources>` with `Resources.Font`, `Resources.Image`, `Resources.Data` children only,
   only as a direct child of `<App>`.
4. Member tags `<Startup.X>` and `<Resources.X>` are accepted **only** for the built-in namespaces
   `Startup` and `Resources` imported by name from `"tessera"`, and only for the members named in
   this spec. Every other member tag stays `TESSERA_JSX_SUBSET_DYNAMIC_TAG`
   ([jsx-subset-v1-jsx.md](jsx-subset-v1-jsx.md) section 10.1). This is the one carve-out
   [ADR-042](../../decisions/ADR-042-jsx-startup-surface.md) records; no dynamic or namespace tags
   are introduced.
5. No other bare package name than `"tessera"` is importable
   ([jsx-subset-v1.md](jsx-subset-v1.md) section 4). The framework package name in source is
   therefore `tessera`, not a vendor-scoped package.

Rejected with a stable code: `Startup.*` or `Resources.*` outside `<App>`; a second `<App>`;
`<App>` not returned by the entry component; `Startup.Loading`/`Startup.Resources` outside a direct
`<App>` child position; declaration elements with unknown props; object-literal or expression
values for `src`; a `Resources.*` outside `Startup.Resources`; a non-element child of
`Startup.Loading`; and a critical/high/low resource id that collides with another declared id.
Codes are `TESSERA_JSX_STARTUP_*` (section 9).

## 5. Launch modes, phases and guarantees

### 5.1 Launch modes and state reuse

Startup is not always a full initialization. Two orthogonal axes decide what the framework may reuse:
the **mode** (what the process and runtime look like at launch) and the **activation** (an external
target that requested this launch).

| Mode | Meaning |
|---|---|
| `cold` | Process start, no previous in-memory state |
| `warm` | Process alive, but the window/surface/UI tree were torn down and must be recreated |
| `hot_resume` | Process, window, UI tree, resources and styles are still valid; the app returns to the foreground |
| `process_restore` | The OS killed the process; the next launch is a cold start with a persisted snapshot |

| Activation | Meaning |
|---|---|
| `none` | User launch without an external target |
| `deep_link` | A validated URL/file/command-line activation ([deep-linking-v1.md](deep-linking-v1.md)) |
| `notification` | A notification tap/action normalized to the same activation event ([services.md](../design/services.md) section 6) |

The six developer-facing scenarios are these axes combined:

| Scenario | Mode | Activation | Behavior |
|---|---|---|---|
| Cold Start | `cold` | `none` | P0-P5 run; the loading screen is the first frame; the app UI mounts at P4 |
| Warm Start | `warm` | `none` (or pending while running) | P0 skipped; recreate only what was destroyed; caches/device reused; restore when `restoreState` |
| Hot Resume | `hot_resume` | `none` | No startup phase, no loading screen, no rebuild; resume presenting and revalidate in the background |
| Process Restore | `process_restore` | `none` | Cold phases, plus a validated snapshot applied before P4 mount |
| Deep Link Launch | `cold`/`warm`/`hot_resume` | `deep_link` | Target parsed before P2, resolved after auth/shell, overrides the restored route |
| Notification Launch | `cold`/`warm`/`hot_resume` | `notification` | Same as deep link; the notification service validates the payload and normalizes it |

Detection inputs, framework-internal and deterministic under test: process liveness; whether the
window/surface/tree are valid; snapshot presence and validity; the pending activation queue; app
version, route table and asset identity; signed-in session identity.

| Reused | `cold` | `warm` | `hot_resume` | `process_restore` |
|---|---|---|---|---|
| OS launch artifact (P0) | yes | no | no | yes |
| GPU device and pipelines | create | reuse if alive, else create | reuse | create |
| CPU/memory caches | empty | reuse | reuse | empty |
| Disk cache | may be warm | reuse | reuse | may be warm |
| UI tree and resources | rebuild | rebuild, then restore | keep | rebuild and restore |
| Restore snapshot | only if valid | only if valid | not read | only if valid |
| Session/auth | revalidate | revalidate | revalidate in background | revalidate |

Mode behavior, normative:

- **Cold**: P0-P5 as in section 5.2. No reuse.
- **Warm**: P0 is skipped (no OS splash). P1 recreates only the destroyed surface/window; the device,
  pipelines and caches are reused when still valid. P2 is required because the window must present
  once; it shows the loading subtree only if a critical resource was evicted, otherwise the restored
  screen appears directly at P4. Restore applies when `restoreState` is true.
- **Hot resume**: no P0-P3, no tree rebuild, no loading screen, no snapshot read; P4 is already
  reached. The framework resumes presenting on demand, revalidates session and stale data in the
  background (stale-while-revalidate), reapplies geometry/DPI/fold changes that happened while away,
  resumes animations under reduced motion, and cancels stale loads. There is no loading screen on
  this path (G13).
- **Process restore**: cold phases with a validated snapshot. Validation covers app version, route
  table, size limits and ids; an invalid, foreign or corrupt snapshot is discarded in favor of the
  previous good snapshot or the route root, never executed and never blocking startup. User-scoped
  state applies only after auth; logout clears it.
- **Deep link / notification**: the base mode is cold, warm or hot resume, plus one pending
  activation. Cold/warm: the activation is parsed and validated before P2 and resolved after
  auth/shell (P3), then overrides the restored route at P4. Hot resume: routed immediately without a
  loading screen, focusing the existing window under the `openIn` policy
  ([deep-linking-v1.md](deep-linking-v1.md) section 6). The pending activation survives readiness
  and auth; in V1 it is bounded to one entry, latest wins, and duplicates are deduped. Notification
  payloads are validated and normalized by the notification service before they become an activation.

Activation target precedence:

1. A pending activation target wins over the restored route: activation is intent, restore is
   continuity.
2. Without an activation, a valid snapshot wins over the default initial UI.
3. Auth-gated targets wait for auth; the pending activation resumes after login and is discarded on
   logout. Destructive actions require confirmation before running.
4. An invalid or unknown target falls back to the not-found route or the route root with a
   diagnostic; it never produces a blank screen or a crash.

### 5.2 Phases

The design pipeline is in [startup-and-loading.md](../design/startup-and-loading.md) section 2; the
ids below are the stable `stage` values used by diagnostics and metrics. Not every mode runs every
phase (section 5.1).

| Id | Phase | Enter when | Content |
|---|---|---|---|
| `launch` (P0) | OS launch screen | Process start | Platform-generated launch artifact only: Android splash theme, iOS launch screen, or (desktop) a hidden window. No app code runs |
| `runtime` (P1) | Minimal native runtime | Launch artifact accepted | Window, graphics surface, chrome policy ([window-chrome-v1.md](window-chrome-v1.md)), device/queues, executors, completion queue, `RuntimeLoop`. No app UI |
| `first_frame` (P2) | First frame | Surface presentable | Framework `WindowChrome` (when `window.titleBar` is `custom`) plus the loading subtree presented once. Independent of app resources |
| `background_init` (P3) | Background initialization | P2 frame presented | Critical set loads; resource manager, remaining fonts, shader/pipeline warm-up, caches, services, session/auth, initial route and restore validation run off the UI thread |
| `interactive` (P4) | Interactive ready | Critical set ready and initial route/restore resolved | App UI tree mounted and laid out (first meaningful frame); input dispatched. `high` resources may still load |
| `optimizing` (P5) | Progressive optimization | P4 reached | `low` prefetch, image variants, cache warm-up, next-route preparation; idle-only and cancellable |

### 5.3 Guarantees

Guarantees, normative:

- **G1 — Always a first frame.** P2 presents the loading subtree without waiting for any app
  resource. If `loading.screen` is `none` and no `Startup.Loading` exists, the first presented
  frame is the themed window background (still a frame; discouraged, section 7).
- **G2 — Nothing app-specific before P2.** The loading subtree is the only app-authored content
  presented in P2; the initial UI tree is not built before P4.
- **G3 — Never block the UI thread.** Loading, decode, upload, session/auth and route work are
  asynchronous; completions apply at a frame boundary under the per-tick budget
  ([resource-lifecycle-v1.md](resource-lifecycle-v1.md), [ADR-021](../../decisions/ADR-021-async-runtime.md)).
  No fence wait, blocking I/O, or infinite acquire on the UI thread.
- **G4 — Critical-only gating.** Only `critical` resources, restore validation and route resolution
  may delay P4. `high` and `low` never delay P2 or P4.
- **G5 — No fake progress.** The default screen is indeterminate. A custom screen shows progress
  only for real stage transitions, never percentages or timers
  ([startup-and-loading.md](../design/startup-and-loading.md) section 4).
- **G6 — No double splash.** P2 is visually continuous with P0 (same `--window-background`/chrome
  record and mark geometry); no second, differently-styled splash appears.
- **G7 — No infinite spinner.** Critical-resource failure follows the bounded retry policy of
  [resource-lifecycle-v1.md](resource-lifecycle-v1.md); after it, P4 is skipped and the root error
  boundary fallback is presented instead of the app UI
  ([error-boundaries-v1.md](error-boundaries-v1.md)), with an actionable retry. A failing custom
  loading subtree falls back to the framework default screen plus a diagnostic; startup never ends
  in a blank window or an endless spinner.
- **G8 — Cancellation.** Closing the window, backgrounding the app, or cancelling startup cancels
  queued and in-flight P3/P5 work; cancellation is not an error
  ([resource-lifecycle-v1.md](resource-lifecycle-v1.md)).
- **G9 — Power-aware.** The default loading screen animates only while work is in flight and falls
  back to a static mark under reduced motion or `powerPolicy: "power-save"`. A settled startup
  presents nothing until input or a wake event ([ADR-019](../../decisions/ADR-019-power-aware-runtime.md)).
- **G10 — Accessibility.** The loading screen is announced once with progress semantics; the app UI
  does not steal focus before P4; reduced motion is respected
  ([accessibility-v1.md](accessibility-v1.md)).
- **G11 — Determinism.** Golden/deterministic mode pins the critical set, the resource order, the
  clock and the load completions, so P0-P5 progression and goldens are reproducible.
- **G12 — Restore before mount.** When `restoreState` is true, restore validation, clamping,
  services/auth, route stack, scroll/focus and drafts apply before P4 mount, never blocking startup
  on non-critical state ([state-restoration-v1.md](state-restoration-v1.md) section on order).
- **G13 — Minimum work per mode.** `hot_resume` runs no startup phase, rebuilds no tree and shows no
  loading screen; `warm` skips P0 and recreates only what was destroyed; only `cold` and
  `process_restore` perform full initialization.
- **G14 — No stale trust.** Warm, hot resume and process restore revalidate session, auth-gated
  routes and stale data before presenting them as current; no stale user data flash
  ([async-data-v1.md](async-data-v1.md)).
- **G15 — Activation is safe and never lost.** Activations are validated, bounded, deduped and
  auth-gated; no destructive action runs without confirmation; malformed payloads are rejected with
  a diagnostic; a valid pending target survives readiness and auth.
- **G16 — Restore is validated and bounded.** A snapshot is validated, migrated, size-capped and
  applied before mount; corrupt or foreign snapshots fall back to the previous good snapshot or the
  route root and never execute.
- **G17 — Reuse is invalidated safely.** A change of app version, route table, asset identity or
  signed-in user downgrades reuse (`hot_resume` -> `warm` -> `cold`) or discards the snapshot
  instead of reusing stale state.
- **G18 — Observable launch.** Every launch records its mode and activation source as structured
  diagnostics and as metric labels (section 9); forcing a mode is a deterministic-test and harness
  capability, not a production app decision.

## 6. The framework default loading screen

The default loading screen is part of the framework, compiled into every generated app, and always
available. "Available" means: even with no `<App>`, an empty `Startup.Resources`, a failed custom
loading subtree, or a legacy project, startup presents a valid first frame.

### 6.1 Presentation

- Full window, themed by `--window-background` of the active theme (the built-in `default` theme
  provides it when the project has none, [window-chrome-v1.md](window-chrome-v1.md) section 3.1),
  with the framework mark as a vector `Path` and an indeterminate progress track.
- No text in V1: text would need an app font, an i18n catalog, and a locale before P3, none of
  which is guaranteed at P2. Visual brand text belongs in the custom loading subtree.
- Responsive: centered mark, does not depend on window size, DPI, safe-area insets, or fold posture.
- Reduced motion: the progress track becomes static.
- In `custom` title-bar mode the screen fills the content area below the framework `WindowChrome`
  ([window-chrome-v1.md](window-chrome-v1.md) section 8.1); the first frame still presents before
  any app resource.
- The screen is not dismissed by time; it is replaced by the app UI exactly at P4 (or by the error
  fallback at G7), and it is not shown at all on `hot_resume` (G13).

### 6.2 Platform artifacts (P0)

Codegen emits the launch artifact from the project's icon and theme background:

- Android: a splash theme (`windowSplashScreenBackground`, `windowSplashScreenAnimatedIcon`) and
  manifest entry in the generated app; the activity starts with the splash until P2.
- iOS: `UILaunchScreen`/launch storyboard generated with the same background and icon.
- Desktop: window created hidden and shown only after the P2 frame is presented, so no white or
  unpainted window is ever visible.

The artifact is generated, not hand-edited, and uses the resolved `{background, foreground, scheme}`
record that P1 chrome and the default screen also use (G6, [window-chrome-v1.md](window-chrome-v1.md)
section 3.3); the same record supplies the system-bar appearance on mobile. Platform backends are
Big Step F work
([mobile-integration.md](../design/mobile-integration.md)).

### 6.3 Accessibility and failure

- The default screen exposes an indeterminate `progressbar` semantic with the accessible name
  "Loading" from the framework string table (localized when the app locale is known at P2, English
  otherwise). It is announced once; repeated announcements follow
  [accessibility-v1.md](accessibility-v1.md).
- A custom loading subtree that does not expose an accessible name or progress semantics is wrapped
  in the framework progressbar container with a diagnostic
  (`TESSERA_STARTUP_LOADING_A11Y`), so accessibility never regresses by omission.
- A custom loading subtree that throws is caught by the root boundary and replaced by the default
  screen; startup continues (`TESSERA_STARTUP_LOADING_FAILED`, error).

## 7. Precedence and project configuration

### 7.1 Sources

| Source | Owns |
|---|---|
| JSX startup declaration (this spec) | Which resources, their priority, the loading subtree, `restoreState` |
| `tessera.json` `loading` ([project-config-v1.md](project-config-v1.md)) | Policy and budgets: `authMode`, `resourcePolicy`, `preload`, `memoryBudgetMb`, `diskCacheMb`, `unloadOnBackground`, `evictOnMemoryPressure`, defaults for the fields below |
| Programmatic `tessera::AppConfig` | Runtime overrides |
| CLI flags | Highest-precedence runtime overrides |

Order: CLI > `AppConfig` > compiled startup declaration > `tessera.json` > framework defaults.
The existing rule (CLI > `AppConfig` > `tessera.json` > defaults) is unchanged; the compiled
declaration sits between the runtime overrides and the project file because it is part of the app
binary.

### 7.2 Overlap rules

1. **Critical set** is the union: JSX `critical` entries plus `loading.criticalResources`, deduped
   by resource id; on a duplicate the JSX entry wins. Dedupe order is deterministic.
2. **Loading screen**: `<Startup.Loading>` replaces the default screen. If it is absent,
   `loading.screen` selects the default presentation: `none` (themed background only, warns
   `TESSERA_STARTUP_SCREEN_NONE`), `progress`/`spinner` (default screen). `shell` and `skeleton`
   describe an app-shaped screen that the framework cannot synthesize; with no custom loading
   subtree they fall back to the default screen and report `TESSERA_STARTUP_SCREEN_NO_COMPONENT`
   (warning).
3. **Restore**: `App restoreState` overrides `restore.enabled`. Setting both differently is
   `TESSERA_STARTUP_CONFIG_SHADOWED` (warning). `navigation.restore` and `loading.restoreRoute`
   keep their own precedence ([project-config-v1.md](project-config-v1.md) validation rule 19).
4. **Preload lists**: `loading.preload` (route ids) is unchanged and separate from JSX `low`
   resource hints; both feed P5 under the same bounds and cancellation rules.
5. **No JSX counterpart**: budgets, auth mode, unload/evict/memory/disk policy, and route restore
   stay config-only. The startup surface adds no budget props.
6. **Activation vs restore**: a pending deep-link/notification target overrides the restored route
   (section 5.1). Config keeps owning route/auth declarations; the activation is an event, not
   configuration.

## 8. Generated artifacts and runtime host contract

This section fixes the shape the compiler and runtime must agree on; exact identifiers are
implementation details owned by [ir-v1.md](ir-v1.md) and [compiler.md](../design/compiler.md) once
the slices land.

- **Startup record.** Declaration elements are hoisted into a component-level `startup` record
  (analogous to the component `environment` field): `{ role: "app", restoreState?, loading?:
  ComponentId, resources: ResourceHint[] }` with `ResourceHint { kind: "font"|"image"|"data", id,
  src, priority }`. Hoisting order is source order; the record is deterministic.
- **No scene nodes.** Declaration elements emit no IR node and no `add_child` call. A lowerer must
  not emit them as children of the App node; only the initial UI tree is emitted.
- **Generated manifest.** `<Entry>.startup.g.h/.cpp` exposes the startup manifest: the resource
  table (fixed array, no allocation), the critical set, the loading factory, the app-root factory
  and the restore flag. It is compiled, deterministic and `constexpr`-friendly.
- **Generated entry.** The app entry point constructs the platform window, the RHI device and the
  `AppHost` from the manifest, then runs the `RuntimeLoop`. The `tessera_codegen` CLI grows an entry
  mode; `--check` compares the generated entry and manifest too.
- **`AppHost`** (new runtime module, `include/tessera/app/`) owns the P1-P5 sequence, the scene
  root, `FramePipeline` and the loading/default-screen factories. It exposes a read-only `Stage`,
  a `request_frame` path, and `std::expected<void, StartupError>` entry points; it never blocks and
  never owns the window, swapchain or GPU device (that stays the platform layer's,
  [ADR-020](../../decisions/ADR-020-window-resize-and-lifecycle.md)).
- **Programmatic parity.** The same contract is reachable from C++ (`tessera::app::StartupManifest`,
  `tessera::app::AppHost`) for hosts that do not use codegen ([ADR-014](../../decisions/ADR-014-project-config-format.md)),
  so the JSX surface is the authoring layer, not a second implementation.

## 9. Diagnostics and metrics

Compile-time codes use the `TESSERA_JSX_STARTUP_<NAME>` namespace and follow
[jsx-subset-v1-jsx.md](jsx-subset-v1-jsx.md) section 13.1 (registry + fixture + golden; lowering
and codegen-owned codes are stage-owned skip entries until implemented). Initial set:

| Code | Severity | Meaning |
|---|---|---|
| `TESSERA_JSX_STARTUP_APP_PLACEMENT` | error | `App` not the entry component's returned root, or more than one |
| `TESSERA_JSX_STARTUP_APP_EMPTY` | error | No UI child under `App` |
| `TESSERA_JSX_STARTUP_APP_IMPLICIT` | warning | Entry has no `App`; implicit App used |
| `TESSERA_JSX_STARTUP_DUPLICATE` | error | Second `Startup.Loading`/`Startup.Resources`/`App` |
| `TESSERA_JSX_STARTUP_LOADING_CHILD` | error | Loading subtree is not one static element |
| `TESSERA_JSX_STARTUP_LOADING_RESOURCE` | error | Loading subtree uses an image that is not `critical` |
| `TESSERA_JSX_STARTUP_RESOURCE_ID` | error | Duplicate or missing `name` |
| `TESSERA_JSX_STARTUP_ASSET` | error | `src` not covered by `assets.fonts`/`assets.images` or over limits |
| `TESSERA_JSX_STARTUP_PRIORITY` | error | Unknown `priority` value |
| `TESSERA_JSX_STARTUP_OUTSIDE_APP` | error | `Startup.*`/`Resources.*` outside `App`/`Startup.Resources` |
| `TESSERA_JSX_STARTUP_NAME_RESERVED` | error | Project component/import named `App`, `Startup` or `Resources` |
| `TESSERA_STARTUP_CONFIG_SHADOWED` | warning | JSX overrides an explicit config value |

Runtime diagnostics and metrics (structured, bounded, disabled paths near-zero cost,
[ADR-022](../../decisions/ADR-022-diagnostics-and-profiling.md)):

- `TESSERA_STARTUP_CRITICAL_FAILED` (error, routed to the root boundary),
  `TESSERA_STARTUP_LOADING_FAILED`, `TESSERA_STARTUP_LOADING_FONT`, `TESSERA_STARTUP_LOADING_A11Y`,
  `TESSERA_STARTUP_BUDGET_MISSED` (warning), `TESSERA_STARTUP_ACTIVATION_INVALID` (warning,
  malformed/unknown target routed to the fallback) and `TESSERA_STARTUP_RESTORE_INVALID` (warning,
  discarded snapshot; startup continues).
- Metrics: `startupMs` (to first meaningful frame), `ttfpMs` (to P2), `ttiMs` (to P4), `resumeMs`
  (`hot_resume` wake to first present), `resourceWaitP95Ms`, the phase timestamps `stage.*` used by
  the trace, and the labels `launch.mode` and `activation.source`
  ([lighthouse-audit-v1.md](lighthouse-audit-v1.md), [diagnostics-config-v1.md](diagnostics-config-v1.md)).
- Budgets are diagnostics, not failures: desktop ≤ 300 ms warm / ≤ 1 s cold, mobile cold ≤ 1.5 s,
  cached auth check ≤ 150 ms, route shell ≤ 100 ms, `hot_resume` first present ≤ one frame interval,
  overridable per app profile ([reference/open-questions.md](../reference/open-questions.md) resolved
  budgets).

## 10. V1 scope and deferred surface

In V1:

- `App`, `Startup.Loading`, `Startup.Resources`, `Resources.Font`, `Resources.Image`,
  `Resources.Data`; the membership carve-out of section 4.4; the phase guarantees; the default
  screen; the generated manifest and entry; the diagnostics above.
- `restoreState` as a boolean only; restore mechanics stay as specified elsewhere.
- The framework shell components `WindowChrome` and `AppBar` ([window-chrome-v1.md](window-chrome-v1.md) section 8).

Deferred (not V1; an addition needs an ADR or a spec revision):

- `useStartupStage()` or any startup hook: it needs the hook manifest and environment lowering
  ([jsx-subset-v1.md](jsx-subset-v1.md) section 7.1), which do not exist yet. Custom loading
  subtrees are static in V1.
- `useLaunchMode()`/`useActivation()` or any other launch-info hook (same hook-manifest dependency);
  a `LaunchMode`/`ActivationSource` value is available to C++ hosts and to the headless harness, and
  to JSX only through that future hook.
- Developer phase callbacks, per-phase handlers, or a lifecycle API; forcing a mode from app code
  (tests/harness only).
- `initialScreen`/component-reference props (rejected because no component-reference value exists
  in the IR; children express the initial UI).
- `Resources.Icon`, `Resources.Media`, `Resources.Route`, `Resources.ShaderPipeline`; shader and
  pipeline warm-up is automatic in P3.
- Multiple apps per process, multiple windows started in one App subtree, and per-window startup
  declarations.
- Resource budget props in JSX (config-only).

## 11. Tests and acceptance

Compile time:

- Accepted-form fixtures for section 3 and section 4, with IR golden containing the hoisted startup
  record and no declaration nodes.
- One rejection fixture per code of section 9 (code, span, message) and the diagnostics registry
  golden.
- Determinism: same source and config produce byte-identical IR, manifest and diagnostics.

Runtime (headless, deterministic; `ManualClock` + CPU sink):

- Phase-order test: P2 presented before any critical resource completes; app UI not built before
  P3; P4 after the critical set and restore; P5 only after P4.
- No-blocking test: no tick exceeds the per-tick budget; completions apply at frame boundaries.
- First-frame and default-screen goldens at two window sizes, two DPI scales, light/dark theme,
  reduced motion.
- Failure test: critical failure → retry → root fallback; never blank, never endless spinner;
  custom loading failure → default screen + diagnostic.
- Cancellation/close during P3/P5; background pause of P5; static idle after settle
  (zero presents).
- Accessibility: default screen progressbar name; wrapper for a custom subtree without semantics.
- Mobile: generated splash theme/launch-screen artifact golden and P0→P2 continuity.
- Chrome: appearance resolution (`auto`/`light`/`dark`), P0 artifact and P1 request use one record,
  the window is shown only after P2, and desktop insets stay zero
  ([window-chrome-v1.md](window-chrome-v1.md) section 11). In `custom` mode the P2 frame includes the
  framework title bar and the loading subtree renders below it.
- Launch-mode matrix (section 5.1), each forced deterministically: cold, warm (reuse device/caches,
  no P0), hot resume (no phase, no loading screen, no presents until wake after settle), process
  restore (valid snapshot applied; corrupt/foreign snapshot discarded), and cold/warm/hot deep-link
  and notification launches (target overrides restored route, survives auth, deduped, invalid target
  falls back). Also: logout clears pending activation and user-scoped restore; reuse invalidation on
  app-version/asset/user change (G17).

## 12. Staged implementation plan

Each slice is independently testable and lands green before the next. Big Steps refer to
[AGENTS.md](../../AGENTS.md#big-steps-group-work-dont-micro-step).

| Slice | Work | Acceptance | Big Step |
|---|---|---|---|
| S1 | This spec + ADR-042 + docs index + the jsx-subset member-tag carve-out + the `loading` reconciliation note | `check_docs.py` clean; no code change | docs |
| S2 | Analyzer: built-in namespace member-tag resolution; startup lowering into the `startup` record + hoisting; diagnostic codes/registry/fixtures; IR golden | accepted/rejected fixtures and IR golden pass | C |
| S3 | Codegen: startup manifest, entry `main()`, `AppHost` skeleton (P1-P4), default loading screen, desktop window show timing and chrome application | headless phase-order + first-frame golden + no-blocking test | C/B2 |
| S4 | `ResourceManager` minimal (lease, priority, critical set, preload, failure/cancel) wired to startup hints | resource-lifecycle tests; failure → root fallback | B2 |
| S5 | Background init, readiness/interactive stage, metrics/budgets, power/idle rules | metrics keys present; budget warning; static idle | B2 |
| S6 | P5 prefetch/optimizing, image variants, next-route preparation; Android/iOS launch artifacts and system-bar appearance; auth gating | mobile splash golden; prefetch cancellation | B2/F |
| S7 | Launch modes: detection, warm/hot resume (lifecycle events, no-reinit path), process restore (validated snapshot), deep-link/notification activation routing and precedence | launch-mode matrix tests of section 11 pass | B2/F |
| S8 | `useStartupStage()`/`useLaunchMode()` via the hook manifest + custom stage UI | manifest/lowering/goldens | C |

Prerequisites: S3 needs the entry/mount seam that does not exist yet (no generated `main()`, no
`AppHost`); S4 needs the resource runtime (`src/rendering/resources/` and `src/core/resource/` are
empty `.gitkeep` directories); S6 needs the mobile platform backend (Big Step F); S7 needs OS
lifecycle events ([os-events-v1.md](os-events-v1.md)), the state-restoration runtime
(`RestoreService`, snapshot read/write) and the deep-link platform adapters
([deep-linking-v1.md](deep-linking-v1.md)). These are stated so the plan is not read as
"compiler-only".

## 13. Open questions

- Whether the implicit App (no `<App>` in source) stays a warning forever or becomes an error once
  the generator can migrate projects.
- Whether `Resources.*` should accept low-resolution/high-resolution variants and a
  `placeholder` in V1.1 for the progressive phase.
- Whether the default loading screen should support a project-provided logo and background override
  without writing a custom subtree.
- Whether `loading.screen` should be deprecated entirely in favor of the JSX surface after one
  release.
- Whether a custom loading subtree may reference route-scoped resources (a scope no spec defines
  yet; see [resource-lifecycle-v1.md](resource-lifecycle-v1.md)).
- Exact budget enforcement wiring (diagnostics warning vs profile-driven `startupMs` threshold).
- Warm-start and process-restore budgets (warm is only required not to run P0; no number is fixed).
- Pending-activation policy beyond V1 one-latest-wins, and notification action/category routing
  ownership between the notification service and the route table.
- Whether hot resume may present a stale frame while revalidating (stale-while-revalidate) or must
  refresh before the first present for auth-gated screens.
- Snapshot encryption/key management for process restore on shared devices (see
  [state-restoration-v1.md](state-restoration-v1.md)).
- Chrome/shell details still open (title-bar height, Windows 10 support, custom-control a11y) live in
  [window-chrome-v1.md](window-chrome-v1.md) section 13.
