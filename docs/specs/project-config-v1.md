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
| `runtime` | object | no | see [runtime-config-v1.md](runtime-config-v1.md) | Loop, FPS, vsync/VRR, dirty rendering |
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
| `gpuFramesInFlight` | `2` | `1..3` |
| `deterministic` | `false` | Forced by golden/headless runs |

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
    "gpuFramesInFlight": 2
  },
  "dependencies": { "includeDirs": [], "linkLibraries": [], "licenseReview": true }
}
```

## Versioning

- Adding optional fields: schema version stays `1`.
- Changing meaning/default of an existing field: bump to `2` and provide `tessera migrate`.
- Removing a field: bump and provide a migration.
