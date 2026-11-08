# Project generator and developer configuration

This guide covers how a developer creates, configures, generates, and builds a Tessera
project. The schema reference is [`docs/specs/project-config-v1.md`](../specs/project-config-v1.md).

Status: design only. Neither `tessera` nor `tesserac` is implemented yet; the commands below are the target contract (see [getting-started.md, section 5](getting-started.md#5-target-application-workflow-not-implemented-yet)). V1 means the first release scope described in [AGENTS.md, Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step) (compiler work is Big Step C).

## What the generator does

Tessera has two binaries:

- **`tessera`** — the project CLI: scaffold, validate, generate, build, run, preview.
- **`tesserac`** — the compiler: JSX/TSX → IR (intermediate representation, [ir-v1.md](../specs/ir-v1.md)) → C++ (invoked by `tessera`; `tesserac` can also be run directly).

The generator is deterministic and incremental:

1. Validate `tessera.json`.
2. Discover components from `ui.sourceDirs` / `ui.include`.
3. Parse each component (Tier 1: React rules, which accept or reject hook usage; Tier 2: bindings, which extract `bind::<id>` annotations; both defined in [compiler.md](../design/compiler.md)).
4. Lower to IR (`src/compiler/ir`).
5. Generate one C++ header/source pair per component.
6. Format with `clang-format` (pinned version).
7. Merge protected regions (developer-written C++ blocks) from the previous generation.
8. Write `bindings.json` and a generation report.

Only components whose input or config hash changed are regenerated.

## Quick start

```bash
tessera new my-app --template dashboard --targets windows,linux
cd my-app
tessera dev          # Wasm/WebGPU preview in the browser
tessera build        # generate + compile native
tessera run
```

## Generated project layout

```text
my-app/
├── tessera.json          # project configuration (source of truth)
├── CMakeLists.txt
├── CMakePresets.json
├── .clang-format
├── src/
│   └── main.cpp          # developer-owned entry point
├── ui/
│   ├── App.tsx           # UI components (JSX/TSX)
│   ├── elements.json     # custom native element declarations
│   └── elements/
│       └── Waterfall3D.cpp
├── generated/
│   ├── App.g.h           # generated, do not edit outside protected regions
│   ├── App.g.cpp
│   ├── elements.g.cpp    # compile-time custom element factory registry
│   ├── elements.d.ts     # typed JSX props/events for custom elements
│   ├── bindings.json     # binding id -> component/file/region
│   └── automation.manifest.json  # uint64 uiId/componentId -> source/debug path
├── assets/
│   ├── fonts/
│   ├── images/
│   └── theme.json
└── tests/
```

`tessera new` never overwrites an existing directory. `tessera generate` never deletes
developer C++ inside protected regions ([ADR-008](../../decisions/ADR-008-protected-regions-and-manifest.md), protected regions and the binding manifest).

## Configuration: `tessera.json`

Minimal example:

```json
{
  "$schema": "https://tessera.dev/schema/project-v1.json",
  "schemaVersion": 1,
  "app": {
    "name": "my-app",
    "version": "0.1.0",
    "entry": "ui/App.tsx",
    "namespace": "myapp",
    "targets": ["windows", "linux"],
    "renderer": "vulkan"
  },
  "window": { "title": "My App", "width": 1280, "height": 800 },
  "ui": {
    "sourceDirs": ["ui"],
    "include": ["**/*.tsx", "**/*.jsx"],
    "hooks": {
      "allowed": ["useState", "useEffect", "useRef", "useAnimatedValue"],
      "strict": true
    },
    "bindings": { "annotation": "// Note: bind::", "manifest": "generated/bindings.json" }
  },
  "codegen": {
    "outDir": "generated",
    "namespace": "myapp::gen",
    "protectedRegions": true,
    "deterministic": true
  },
  "build": { "preset": "dev", "staticRuntime": true, "compilerCache": "auto" },
  "dev": { "preview": { "backend": "webgpu", "port": 5173, "hotReload": true } },
  "runtime": {
    "fixedTimestepHz": 60,
    "maxFps": 0,
    "vsync": "on",
    "dynamicRefreshRate": "auto",
    "dirtyRendering": "auto",
    "renderOnDemand": true,
    "powerPolicy": "adaptive",
    "adaptiveQuality": true,
    "resizeMode": "adaptive",
    "asyncWorkerThreads": 0,
    "asyncMainThreadBudgetMs": 2.0
  },
  "diagnostics": {
    "level": "info",
    "sinks": ["debugger", "ring"],
    "tracing": false,
    "overlay": false,
    "crashDumps": true,
    "telemetry": false
  }
}
```

The full field list, defaults, and validation rules are in
[`docs/specs/project-config-v1.md`](../specs/project-config-v1.md). Runtime pacing, FPS cap,
vsync/VRR, render-on-demand, dirty rendering, resize, and async are defined in
[`docs/specs/runtime-config-v1.md`](../specs/runtime-config-v1.md). Buffer defaults and the
user-tunable allowlist are in [`docs/specs/buffer-config-v1.md`](../specs/buffer-config-v1.md)
(how-to: [`docs/guides/tunables.md`](tunables.md)). Loading/auth/resource policy is in
[`docs/specs/resource-lifecycle-v1.md`](../specs/resource-lifecycle-v1.md) and
[`docs/design/startup-and-loading.md`](../design/startup-and-loading.md). Deep links/platform
activation are in [`docs/specs/deep-linking-v1.md`](../specs/deep-linking-v1.md) and
[`docs/guides/deep-linking.md`](deep-linking.md). Logging/tracing/profiling/crash are defined
in [`docs/specs/diagnostics-config-v1.md`](../specs/diagnostics-config-v1.md).

## C++ API for developers

The generated `src/main.cpp` is intentionally thin:

```cpp
#include <tessera/runtime/app.hpp>

int main(int argc, char** argv) {
    tessera::AppConfig config = tessera::AppConfig::from_file("tessera.json");
    tessera::App app(config);
    return app.run();
}
```

Programmatic overrides (C++ code changing config values) are allowed before `run()`:

```cpp
tessera::AppConfig config = tessera::AppConfig::from_file("tessera.json");
config.window.width = 1600;
config.window.height = 900;
config.dev.preview.hot_reload = false;
config.runtime.max_fps = 60;
config.runtime.dynamic_refresh_rate = tessera::RefreshMode::Auto;
config.runtime.render_on_demand = true;
config.runtime.resize_mode = tessera::ResizeMode::Adaptive;
config.diagnostics.level = tessera::LogLevel::Debug;
config.diagnostics.overlay = true;

tessera::App app(config);
return app.run();
```

The config API (`tessera::AppConfig`) mirrors `tessera.json` field-for-field. Unknown fields are rejected. Command
line flags (`--preset`, `--renderer`, `--no-hot-reload`) override the file for one invocation
and never rewrite it.

## Writer-side API: components, hooks, bindings

V1 supports a deliberately small hooks subset ([ADR-036](../../decisions/ADR-036-own-jsx-frontend-and-react-like-model.md), own JSX frontend and React-like model):

| Hook | Binding annotation | Generated C++ |
|---|---|---|
| `useState(init)` | `// Note: bind::<id>` | member variable + getter/setter |
| `useEffect(fn, deps)` | `// Note: bind::<id>` | `OnInitialized` / `OnDataChanged` stubs |
| `useRef(null)` | `// Note: bind::<id>` | pointer to a scene node |
| `useAnimatedValue(init)` | `// Note: bind::<id>` | `AnimationController` / `Tween` |

Rules:

- `ui.hooks.strict = true` makes a known hook missing from `ui.hooks.allowed` an error; `false` makes it a
  warning. Unknown and custom hooks are always errors ([`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 7).
- A `bind::<id>` must be unique across the project; duplicates are hard errors.
- A binding that disappears from the UI produces a migration warning; it is never silently
  deleted from the developer's C++.
- Custom hooks, context, suspense, and dynamic component types are rejected in V1.

## Generated code contract

Ownership, protected regions, and regeneration rules are in
[`developer-workflow.md`](developer-workflow.md); the IR contract is in
[`ir-v1.md`](../specs/ir-v1.md). Config keys were designed in [ADR-014](../../decisions/ADR-014-project-config-format.md) (project config format).

- One header/source pair per component (`<Component>.g.h`, `<Component>.g.cpp`).
- File header: `// Generated by Tessera. Do not edit outside protected regions.`
- Developer logic lives only inside:
  `// [DEVELOPER_LOGIC_START: bind::<id>] ... // [DEVELOPER_LOGIC_END: bind::<id>]`
- Output is formatted with a pinned `clang-format` configuration.
- Output is deterministic: stable component order, no timestamps, no absolute paths, no
  random identifiers.
- Numeric `UiId`/`ComponentId` (build-time stable 64-bit automation ids) are deterministic; `automation.manifest.json` is emitted and
  reviewed like generated code. See [`automation-ids-v1.md`](../specs/automation-ids-v1.md).
- `ui/elements.json` generates `elements.g.cpp` (compile-time factory registry) and
  `elements.d.ts` (typed JSX props/events); no runtime registration/dynamic loading. See
  [`custom-elements-v1.md`](../specs/custom-elements-v1.md).
- Generated files compile with `/W4` (MSVC) and `-Wall -Wextra -Wpedantic` (GCC/Clang).

## Incremental generation

The generator hashes:

- the component source
- the `tessera.json` sections that affect that component (exact list not specified here or in [`project-config-v1.md`](../specs/project-config-v1.md))
- the generator/compiler version
- the formatting configuration

If the hash is unchanged, the component is skipped. `tessera generate --force` regenerates
everything. The report lists `generated`, `skipped`, `merged`, and `warnings`.

## CLI reference (v1)

```text
tessera new <name> [--template blank|dashboard] [--targets windows,linux]
tessera generate [--force] [--check]
tessera dev [--port N] [--no-hot-reload]
tessera build [--preset dev|release] [--target <platform>]
tessera run [--preset dev|release]
tessera doctor
tessera add component <Name>
tessera migrate --to <schemaVersion>
```

Exit codes: `0` success, `1` build/generation failure, `2` invalid configuration or CLI
usage. `tessera doctor` checks toolchain, Vulkan SDK, fonts, and static-linking prerequisites (the full check list is not specified in any doc yet).

## What the generator does not do

- It does not embed a React runtime in production; production is pure C++.
- It does not execute arbitrary JavaScript from components.
- It does not support a dynamic plugin ABI (static linking, [ADR-012](../../decisions/ADR-012-static-linking.md)).
- It does not silently change developer C++ outside protected regions.
