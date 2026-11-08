# `tessera.json` — project configuration schema v1

Status: draft for Big Step C.
File: `tessera.json` at the project root. JSON is the source of truth; the C++ API
`tessera::AppConfig` mirrors it.

## Top-level

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `$schema` | string | no | — | Editor schema URL |
| `schemaVersion` | integer | yes | — | Must be `1` |
| `app` | object | yes | — | Application identity and targets |
| `window` | object | no | see below | Initial window |
| `ui` | object | yes | — | Component discovery and hooks |
| `codegen` | object | no | see below | Generated C++ options |
| `assets` | object | no | empty | Fonts, images, theme |
| `build` | object | no | see below | CMake and toolchain |
| `dev` | object | no | see below | Browser preview |
| `runtime` | object | no | see [runtime-config-v1.md](runtime-config-v1.md) | Loop, FPS, vsync/VRR, dirty rendering, resize, async |
| `loading` | object | no | see [resource-lifecycle-v1.md](resource-lifecycle-v1.md) | Startup/loading/auth/route and lazy/preload/unload policy |
| `deepLinks` | object | no | see [deep-linking-v1.md](deep-linking-v1.md) | External URL/file/command-line activation |
| `navigation` | object | no | see [navigation-v1.md](navigation-v1.md) | Route table, back stack, restore policy |
| `restore` | object | no | see [state-restoration-v1.md](state-restoration-v1.md) | Versioned route/window/scroll/draft restore |
| `i18n` | object | no | see [i18n-v1.md](i18n-v1.md) | Locales, catalogs, fallback, pseudo-locale |
| `updates` | object | no | see [updates-and-distribution-v1.md](updates-and-distribution-v1.md) | Signed update channel/manifest/apply policy |
| `buffers` | object | no | see [buffer-config-v1.md](buffer-config-v1.md) | Engine buffer/cache/queue defaults |
| `tunables` | array | no | `[]` | Explicit allowlist of user-adjustable settings |
| `diagnostics` | object | no | see [diagnostics-config-v1.md](diagnostics-config-v1.md) | Logging, tracing, profiling, crash |
| `dependencies` | object | no | empty | Include/link additions |

Unknown fields are **errors** (strict schema). Paths are relative to the project root; absolute
paths are rejected.

## `app`

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `name` | string | yes | — | `^[a-z][a-z0-9-]*$` |
| `version` | string | yes | — | SemVer |
| `entry` | string | yes | — | Must exist and end in `.tsx`/`.jsx` |
| `namespace` | string | yes | — | C++ namespace root, `^[a-z][a-z0-9_]*$` |
| `targets` | string[] | yes | — | Subset of `windows`, `linux`; `macos`/`android` later |
| `renderer` | string | yes | — | `vulkan` in V1 |

## `window`

| Field | Type | Default | Notes |
|---|---|---|---|
| `title` | string | app name | Window title |
| `width` | integer | `1280` | > 0 |
| `height` | integer | `800` | > 0 |
| `scale` | string | `auto` | `auto` or a number (DPI scale) |
| `resizable` | boolean | `true` | |

## `ui`

| Field | Type | Required | Default | Notes |
|---|---|---|---|---|
| `sourceDirs` | string[] | yes | — | Directories containing components |
| `include` | string[] | no | `["**/*.tsx", "**/*.jsx"]` | Glob patterns |
| `exclude` | string[] | no | `["**/*.test.*", "**/node_modules/**"]` | Glob patterns |
| `hooks.allowed` | string[] | yes | — | Subset of `useState`, `useEffect`, `useRef`, `useAnimatedValue` |
| `hooks.strict` | boolean | no | `true` | Reject unknown hooks |
| `bindings.annotation` | string | no | `// Note: bind::` | Marker prefix |
| `bindings.manifest` | string | no | `generated/bindings.json` | Must stay inside `codegen.outDir` |

## `codegen`

| Field | Type | Default | Notes |
|---|---|---|---|
| `outDir` | string | `generated` | Must not overlap `ui.sourceDirs` |
| `includeDir` | string | `generated/include` | Public generated headers |
| `namespace` | string | `<app.namespace>::gen` | Generated C++ namespace |
| `language` | string | `c++20` | `c++20` only in V1 |
| `format` | string | `clang-format` | `clang-format` or `none` (debug only) |
| `protectedRegions` | boolean | `true` | Must be `true` outside debug |
| `deterministic` | boolean | `true` | Must be `true` outside debug |
| `headerSuffix` | string | `.g.h` | |
| `sourceSuffix` | string | `.g.cpp` | |

## `assets`

| Field | Type | Default | Notes |
|---|---|---|---|
| `fonts` | string[] | `[]` | Committed font files (OFL/FTL licenses preserved) |
| `images` | string[] | `[]` | Directories or files |
| `theme` | string | `null` | Path to a theme JSON |

## `build`

| Field | Type | Default | Notes |
|---|---|---|---|
| `system` | string | `cmake` | `cmake` in V1 |
| `generator` | string | `ninja` | `ninja` or a Visual Studio generator |
| `preset` | string | `dev` | Name from `CMakePresets.json` |
| `staticRuntime` | boolean | `true` | Must be `true` (ADR-012) |
| `compilerCache` | string | `auto` | `auto`, `ccache`, `sccache`, `off` |
| `sanitizers` | string[] | `[]` | Subset of `address`, `undefined`, `thread` |

## `dev`

| Field | Type | Default | Notes |
|---|---|---|---|
| `preview.backend` | string | `webgpu` | `webgpu` only in V1 |
| `preview.port` | integer | `5173` | |
| `preview.hotReload` | boolean | `true` | |

## `runtime`

See [runtime-config-v1.md](runtime-config-v1.md). Key defaults:

| Field | Default | Notes |
|---|---|---|
| `fixedTimestepHz` | `60` | Simulation tick rate; deterministic |
| `maxFps` | `0` | `0` = uncapped (display/vsync limits) |
| `vsync` | `on` | `off`, `on`, `adaptive`, `mailbox` |
| `dynamicRefreshRate` | `auto` | VRR when available, safe fallback |
| `framePacing` | `auto` | `auto`, `smooth`, `low-latency`, `power-save` |
| `dirtyRendering` | `auto` | Dirty style/layout/paint/render + damage |
| `renderOnDemand` | `true` | Render only on wake/dirty events; static UI idles |
| `powerPolicy` | `adaptive` | Power/quality preset; see runtime spec |
| `adaptiveQuality` | `true` | Reduce optional cost on missed budgets |
| `pauseWhenOccluded` | `true` | No GPU work when hidden/minimized |
| `resizeMode` | `adaptive` | `live`, `deferred`, `adaptive` |
| `asyncWorkerThreads` | `0` | `0` = auto |
| `asyncMainThreadBudgetMs` | `2.0` | Per-tick completion budget |
| `gpuFramesInFlight` | `2` | `1..3` |
| `deterministic` | `false` | Forced by golden/headless runs |

## `loading`

Startup/loading/auth and resource policy. See
[`startup-and-loading.md`](../design/startup-and-loading.md),
[`resource-lifecycle-v1.md`](resource-lifecycle-v1.md), and
[`loading-matrix.md`](../guides/loading-matrix.md).

```json
"loading": {
  "criticalResources": ["theme.default", "font.inter.regular"],
  "screen": "skeleton",
  "authMode": "blocking",
  "restoreRoute": true,
  "resourcePolicy": "lazy",
  "preload": ["route.home"],
  "memoryBudgetMb": 256,
  "diskCacheMb": 512,
  "unloadOnBackground": true,
  "evictOnMemoryPressure": true
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `criticalResources` | string[] | `[]` | Minimal eager set for first meaningful frame |
| `screen` | string | `skeleton` | `none`, `shell`, `skeleton`, `progress`, `spinner` |
| `authMode` | string | `blocking` | `blocking`, `overlay`, `deferred`, `background` |
| `restoreRoute` | boolean | `true` | Restore last route/deep link after auth |
| `resourcePolicy` | string | `lazy` | `lazy` (default); explicit preload list |
| `preload` | string[] | `[]` | Route/resource ids to preload on intent/idle |
| `memoryBudgetMb` | integer | `256` | Runtime resource cache budget |
| `diskCacheMb` | integer | `512` | Disk cache budget; `0` disables |
| `unloadOnBackground` | boolean | `true` | Release resources when backgrounded |
| `evictOnMemoryPressure` | boolean | `true` | Evict idle/evictable resources on pressure |

Rules: `criticalResources` stays small; `preload` is bounded/cancellable; user settings may
adjust budgets only within hard caps; golden/deterministic mode pins the critical set and load
order and ignores network/system fonts.

Restore precedence: `restore.enabled` is the master switch. `navigation.restore` controls the
route stack; `loading.restoreRoute` only resumes a pending deep-link/activation after auth.
A false master disables both.

## `deepLinks`

External URL/file/command-line activation. See
[`deep-linking-v1.md`](deep-linking-v1.md) and [`guides/deep-linking.md`](../guides/deep-linking.md).

```json
"deepLinks": {
  "schemes": ["myapp"],
  "hosts": ["app.example.com"],
  "routes": [
    { "pattern": "/orders/:id", "route": "orders.detail", "auth": true },
    { "pattern": "/share", "route": "share", "auth": false, "confirm": true }
  ],
  "fileTypes": ["myapp"],
  "singleInstance": true,
  "openIn": "existing",
  "allowColdStart": true,
  "fallbackUrl": "https://app.example.com",
  "maxUrlLength": 4096
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `schemes` | string[] | `[]` | Custom schemes; lowercase, no spaces |
| `hosts` | string[] | `[]` | HTTPS App/Universal Link hosts; no wildcards |
| `routes` | Route[] | `[]` | `pattern`, `route`, `auth`, `confirm` |
| `fileTypes` | string[] | `[]` | Extensions/UTIs/MIME |
| `singleInstance` | boolean | `true` | Forward to existing process |
| `openIn` | string | `existing` | `existing` or `newWindow` |
| `allowColdStart` | boolean | `true` | Allow launch via link |
| `fallbackUrl` | string | — | HTTPS web fallback |
| `maxUrlLength` | integer | `4096` | Reject oversized input |

## `navigation`

```json
"navigation": {
  "routesFile": "ui/routes.json",
  "notFound": "not_found",
  "restore": true,
  "modalStack": "global"
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `routesFile` | string | `ui/routes.json` | Static route table; path inside project |
| `notFound` | string | `not_found` | Route id for unknown paths |
| `restore` | boolean | `true` | Restore route stack on launch; requires `restore.enabled` |
| `modalStack` | string | `global` | `global` or `nested` |

See [`navigation-v1.md`](navigation-v1.md) for route/guard/back/deep-link semantics.

## `restore`

```json
"restore": {
  "enabled": true,
  "maxBytes": 262144,
  "persistRouteState": true,
  "persistScroll": true,
  "persistDrafts": true,
  "persistSecure": false
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `enabled` | boolean | `true` | Master switch; disables navigation/route continuation too |
| `maxBytes` | integer | `262144` | Per-window snapshot cap |
| `persistRouteState` | boolean | `true` | Opt-in per route still applies |
| `persistScroll` | boolean | `true` | Keyed by route/list id |
| `persistDrafts` | boolean | `true` | Secure fields excluded |
| `persistSecure` | boolean | `false` | Must remain false for secure fields |

See [`state-restoration-v1.md`](state-restoration-v1.md).

## `i18n`

```json
"i18n": {
  "defaultLocale": "en-US",
  "locales": ["en-US", "vi-VN", "ja-JP"],
  "fallbackLocale": "en-US",
  "catalogDir": "locales",
  "pseudoLocale": "en-XA",
  "timezone": "platform"
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `defaultLocale` | string | `en-US` | BCP-47; must be in `locales` |
| `locales` | string[] | `[defaultLocale]` | Packaged catalogs |
| `fallbackLocale` | string | `defaultLocale` | Deterministic fallback |
| `catalogDir` | string | `locales` | Project-relative; no traversal |
| `pseudoLocale` | string | `en-XA` | Test-only pseudo-localization |
| `timezone` | string | `platform` | `platform`, `UTC`, or IANA id |

See [`i18n-v1.md`](i18n-v1.md).

## `updates`

```json
"updates": {
  "enabled": true,
  "channel": "stable",
  "manifestUrl": "https://updates.example.com/manifest.json",
  "publicKey": "ed25519:...",
  "checkOnStart": true,
  "autoDownload": true,
  "autoApply": "onRestart"
}
```

| Field | Type | Default | Notes |
|---|---|---|---|
| `enabled` | boolean | `false` | Off unless explicitly enabled |
| `channel` | string | `stable` | `stable`, `beta`, `nightly` |
| `manifestUrl` | string | — | HTTPS allowlisted host |
| `publicKey` | string | — | Signature verification key |
| `checkOnStart` | boolean | `true` | Background check, never blocks startup |
| `autoDownload` | boolean | `true` | Resumable/background |
| `autoApply` | string | `onRestart` | `onRestart`, `prompt`, `manual` |

See [`updates-and-distribution-v1.md`](updates-and-distribution-v1.md).

## `buffers`

Developer defaults for engine buffers. See
[buffer-config-v1.md](buffer-config-v1.md). Existing runtime fields
(`layerCacheBudgetMb`, `drawListCacheBudgetMb`, `asyncQueueCapacity`, `gpuFramesInFlight`)
remain under `runtime`; do not duplicate them here.

```json
"buffers": {
  "renderer": {
    "frameArenaBytes": 8388608,
    "vertexInitialVertices": 4096,
    "vertexMaxVertices": 262144,
    "stagingRingBytes": 16777216,
    "glyphAtlasBytes": 33554432,
    "textureCacheBytes": 134217728
  },
  "input": { "queueCapacity": 2048 },
  "ui": { "virtualListPoolCap": 256, "virtualListOverscanPx": 200 },
  "budgetBytes": 536870912
}
```

Rules: values are defaults, not user settings; sizes are aligned/rounded by the engine; total
budgets must not exceed `budgetBytes`; zero disables optional caches only where documented.

## `tunables`

Explicit allowlist of values users may change. See
[buffer-config-v1.md](buffer-config-v1.md) and
[`guides/tunables.md`](../guides/tunables.md).

```json
"tunables": [
  {
    "id": "renderer.stagingRingBytes",
    "type": "bytes",
    "labelKey": "settings.upload_buffer",
    "descriptionKey": "settings.upload_buffer.description",
    "min": 4194304,
    "max": 134217728,
    "step": 4194304,
    "scope": "restartRenderer",
    "category": "graphics",
    "powerImpact": "low"
  }
]
```

Rules: `id` must resolve to a real buffer/runtime value; `min <= default <= max`; `step > 0`;
`max` must not exceed the engine hard cap; `scope` is `live`, `nextFrame`,
`restartRenderer`, or `restartApp`; only listed ids are user-tunable.

## `diagnostics`

See [diagnostics-config-v1.md](diagnostics-config-v1.md). Key defaults:

| Field | Default | Notes |
|---|---|---|
| `level` | `info` | `trace`/`debug`/`info`/`warn`/`error`/`fatal` |
| `sinks` | `["debugger","ring"]` | File/console/ETW/Perfetto optional |
| `tracing` | `false` | Span tracing; file/Perfetto/Tracy sinks |
| `gpuTimestamps` | `false` | Requires backend support |
| `overlay` | `false` | Dev overlay |
| `validation` | `auto` | On in debug |
| `crashDumps` | `true` | Bounded, redacted |
| `telemetry` | `false` | Opt-in only |

## `dependencies`

| Field | Type | Default | Notes |
|---|---|---|---|
| `includeDirs` | string[] | `[]` | Added to the generated CMake target |
| `linkLibraries` | string[] | `[]` | Must be permissive and static-linkable (ADR-012/013) |
| `licenseReview` | boolean | `true` | Must be `true`; `tessera doctor` checks notices |

## Validation rules

1. `schemaVersion` must be `1`.
2. `app.entry` must exist and be a component file.
3. `app.renderer` must be `vulkan` in V1.
4. `ui.hooks.allowed` must be a subset of the supported hooks.
5. `bindings.manifest` must be under `codegen.outDir`.
6. `codegen.outDir` must not be inside `ui.sourceDirs`.
7. `build.staticRuntime` must be `true`.
8. `dependencies.linkLibraries` must be reviewed; no GPL/LGPL in statically linked code.
9. Unknown fields, absolute paths, and duplicate binding ids are errors.
10. `tessera generate --check` fails if generated output differs from the working tree.
11. `runtime` follows [runtime-config-v1.md](runtime-config-v1.md) validation; `deterministic`
    is forced for golden/headless runs.
12. `diagnostics` follows [diagnostics-config-v1.md](diagnostics-config-v1.md) validation;
    telemetry is opt-in and overlay is not a release default.
13. `buffers` follows [buffer-config-v1.md](buffer-config-v1.md): no duplicate values already
    owned by `runtime`, budgets are respected, and sizes are within engine hard caps.
14. `tunables` ids must resolve to a real buffer/runtime value; `min <= default <= max`,
    `step > 0`, and `max` is within the engine hard cap.
15. User settings are stored outside `tessera.json`; they may override only exposed tunable
    ids and never rewrite the project file. Deterministic/golden mode ignores user tunables.
16. `loading` follows [resource-lifecycle-v1.md](resource-lifecycle-v1.md): `criticalResources`
    is a small explicit set, `preload` is bounded/cancellable, and unload/eviction are enabled
    by policy. Deterministic mode pins the critical set and load order.
17. `deepLinks` follows [deep-linking-v1.md](deep-linking-v1.md): schemes/hosts/routes are
    unique allowlists, route patterns resolve to real routes, `fallbackUrl` is HTTPS, and no
    secrets/tokens are allowed in URLs.
18. `navigation` follows [navigation-v1.md](navigation-v1.md): routes are unique, `notFound`
    resolves, and modal stack policy is known.
19. `restore` follows [state-restoration-v1.md](state-restoration-v1.md): size is bounded,
    secure fields are never persisted, and unknown versions fall back safely. Precedence:
    `restore.enabled` > `navigation.restore` > `loading.restoreRoute`.
20. `i18n` follows [i18n-v1.md](i18n-v1.md): locales are valid BCP-47, catalogs exist, fallback
    is deterministic, and catalogs are compiled/validated at build time.
21. `updates` follows [updates-and-distribution-v1.md](updates-and-distribution-v1.md): HTTPS
    manifest/allowlist, signature key present, channel known, and devtools/updates are
    release-disabled by default.

## Example

```json
{
  "$schema": "https://tessera.dev/schema/project-v1.json",
  "schemaVersion": 1,
  "app": {
    "name": "factory-dashboard",
    "version": "0.1.0",
    "entry": "ui/App.tsx",
    "namespace": "factory",
    "targets": ["windows", "linux"],
    "renderer": "vulkan"
  },
  "window": { "title": "Factory Dashboard", "width": 1440, "height": 900 },
  "ui": {
    "sourceDirs": ["ui"],
    "include": ["**/*.tsx"],
    "exclude": ["**/*.test.tsx"],
    "hooks": {
      "allowed": ["useState", "useEffect", "useRef", "useAnimatedValue"],
      "strict": true
    },
    "bindings": { "annotation": "// Note: bind::", "manifest": "generated/bindings.json" }
  },
  "codegen": {
    "outDir": "generated",
    "includeDir": "generated/include",
    "namespace": "factory::gen",
    "language": "c++20",
    "format": "clang-format",
    "protectedRegions": true,
    "deterministic": true
  },
  "assets": {
    "fonts": ["assets/fonts/Inter-Regular.ttf"],
    "images": ["assets/images"],
    "theme": "assets/theme.json"
  },
  "build": {
    "system": "cmake",
    "generator": "ninja",
    "preset": "dev",
    "staticRuntime": true,
    "compilerCache": "auto",
    "sanitizers": []
  },
  "dev": { "preview": { "backend": "webgpu", "port": 5173, "hotReload": true } },
  "runtime": {
    "fixedTimestepHz": 60,
    "maxFps": 0,
    "vsync": "on",
    "dynamicRefreshRate": "auto",
    "dirtyRendering": "auto",
    "renderOnDemand": true,
    "powerPolicy": "adaptive",
    "resizeMode": "adaptive",
    "asyncWorkerThreads": 0,
    "asyncMainThreadBudgetMs": 2.0,
    "gpuFramesInFlight": 2
  },
  "loading": {
    "criticalResources": ["theme.default", "font.inter.regular"],
    "screen": "skeleton",
    "authMode": "blocking",
    "restoreRoute": true,
    "resourcePolicy": "lazy",
    "preload": ["route.home"],
    "memoryBudgetMb": 256,
    "diskCacheMb": 512,
    "unloadOnBackground": true,
    "evictOnMemoryPressure": true
  },
  "deepLinks": {
    "schemes": ["factory"],
    "hosts": ["app.example.com"],
    "routes": [
      { "pattern": "/orders/:id", "route": "orders.detail", "auth": true },
      { "pattern": "/share", "route": "share", "auth": false, "confirm": true }
    ],
    "fileTypes": ["factory"],
    "singleInstance": true,
    "openIn": "existing",
    "allowColdStart": true,
    "fallbackUrl": "https://app.example.com"
  },
  "buffers": {
    "renderer": { "stagingRingBytes": 16777216, "glyphAtlasBytes": 33554432 },
    "ui": { "virtualListPoolCap": 256, "virtualListOverscanPx": 200 },
    "budgetBytes": 536870912
  },
  "tunables": [
    {
      "id": "renderer.stagingRingBytes",
      "type": "bytes",
      "labelKey": "settings.upload_buffer",
      "min": 4194304,
      "max": 134217728,
      "step": 4194304,
      "scope": "restartRenderer",
      "category": "graphics",
      "powerImpact": "low"
    }
  ],
  "diagnostics": {
    "level": "info",
    "sinks": ["debugger", "ring"],
    "tracing": false,
    "overlay": false,
    "crashDumps": true,
    "telemetry": false
  },
  "dependencies": { "includeDirs": [], "linkLibraries": [], "licenseReview": true }
}
```

## Versioning

- Adding optional fields: schema version stays `1`.
- Changing meaning/default of an existing field: bump to `2` and provide `tessera migrate`.
- Removing a field: bump and provide a migration.
