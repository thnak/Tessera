# Project generator and developer configuration

This guide covers how a developer creates, configures, generates, and builds a Tessera
project. The schema reference is [`docs/specs/project-config-v1.md`](../specs/project-config-v1.md).

Status: partly implemented. **`tessera new` ships** (issue #77, first slice): it scaffolds a project
from the `blank` or `counter` template, refuses a non-empty directory, and downloads nothing. The
remaining commands below (`generate`, `dev`, `build`, `run`, `doctor`, `add`, `migrate`) and the
prebuilt-toolchain route are still the target contract — see
[getting-started.md, section 5](getting-started.md#5-starting-an-app-tessera-new-and-the-target-workflow).
V1 means the first release scope described in [AGENTS.md, Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step) (compiler work is Big Step C).

## What the generator does

Tessera has two binaries:

- **`tessera`** — the project CLI: scaffold, validate, generate, build, run, preview. Today it is the
  headless automation harness (`tessera test`, `tessera headless`) plus `tessera new`; the other
  verbs are design only.
- **`tesserac`** — the compiler: JSX/TSX → IR (intermediate representation, [ir-v1.md](../specs/ir-v1.md)) → C++ (invoked by `tessera`; `tesserac` can also be run directly). Today this is the `tessera_codegen` binary.

The generator is deterministic and incremental:

1. Validate `tessera.json`.
2. Discover components from `ui.sourceDirs` / `ui.include`.
3. Parse each component (Tier 1: React rules, which accept or reject hook usage; Tier 2: bindings, which extract `bind::<id>` annotations; both defined in [compiler.md](../design/compiler.md)).
4. Lower to IR (`src/compiler/ir`).
5. Generate the `<Component>.g.hpp` base for each component (ADR-040).
6. Format with `clang-format` (pinned version).
7. Scaffold `<Component>.hpp/.cpp` only where they do not exist; never rewrite a developer file.
8. Write `automation.manifest.json` and a generation report.

Only components whose input or config hash changed are regenerated.

## Quick start

`tessera new` works today; the three commands after it are the target contract.

```bash
tessera new my-app --template blank     # works: scaffolds a project (issue #77)
cd my-app
tessera dev          # design only: Wasm/WebGPU preview in the browser
tessera build        # design only: generate + compile native
tessera run          # design only
```

Until `generate`/`build`/`run` land, build the scaffold with its own presets against a Tessera
checkout — the generated `README.md` gives the exact commands:

```bash
cmake --preset dev -DTESSERA_SOURCE_DIR=/path/to/Tessera
cmake --build --preset dev
./build/dev/my-app --frames 120
```

### What `tessera new` does and does not do

```text
tessera new <dir> [--template blank|counter] [--title TEXT]
tessera new --list-templates
```

It writes 12 files: `tessera.json`, `CMakeLists.txt`, `CMakePresets.json`, `.clang-format`,
`.gitignore`, `README.md`, `ui/App.tsx`, `ui/app.css`, `ui/elements.json`, `src/main.cpp`,
`src/App.hpp`, `src/App.cpp`. Exit codes: 0 created, 1 write failure, 2 usage or a non-empty target
directory.

- **Never overwrites.** A target that exists and holds any entry is refused before the first byte is
  written; there is no `--force` and no merge. A missing directory and an empty one are both accepted.
- **Deterministic.** The output is a function of (template, directory name, `--title`) alone: no
  timestamp, no absolute path, no random id, POSIX separators and LF endings on every platform. Two
  runs with the same inputs are byte-identical, which `new_project` asserts.
- **Downloads nothing and runs nothing.** No network, no `git init`, no package manager. The project
  builds against an installed Tessera SDK (route A, not released) or a Tessera checkout (route B,
  which works today).
- **No assets.** No font and no theme file: both are licensed third-party assets. `src/main.cpp`
  looks for `assets/fonts/Inter-Regular.ttf` then `assets/fonts/NotoSans-Regular.ttf`, and takes
  `--font <path>`.

## Generated project layout

```text
my-app/
├── tessera.json          # project configuration (source of truth)
├── CMakeLists.txt
├── CMakePresets.json
├── .clang-format
├── src/
│   ├── main.cpp          # developer-owned entry point
│   └── App.hpp/.cpp      # developer-owned class deriving from gen::AppBase<App>
├── ui/
│   ├── App.tsx           # UI components (JSX/TSX)
│   ├── elements.json     # custom native element declarations
│   └── elements/
│       └── Waterfall3D.cpp
├── generated/
│   ├── App.g.hpp         # generated base (ADR-040), never edited, not committed
│   ├── elements.g.cpp    # compile-time custom element factory registry
│   ├── elements.d.ts     # typed JSX props/events for custom elements
│   └── automation.manifest.json  # uint64 uiId/componentId -> source/debug path
├── assets/
│   ├── fonts/
│   ├── images/
│   └── theme.json
└── tests/
```

`tessera new` never overwrites an existing directory. `tessera generate` never rewrites a developer
class ([ADR-040](../../decisions/ADR-040-generated-base-and-developer-class.md), generated base plus
developer-owned class).

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
  warning. Unknown hooks are always errors ([`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 7);
  a project-declared `use...` function is a custom hook and is expanded, not rejected
  ([`custom-hooks-v1.md`](../specs/custom-hooks-v1.md)).
- A `bind::<id>` must be unique across the project; duplicates are hard errors.
- A binding that disappears from the UI leaves an unused member: the generator reports
  `TESSERA_CG_BINDING_UNUSED`; it is never silently deleted from the developer's C++.
- Context, suspense, and dynamic component types are rejected in V1. Custom hooks are expanded at
  each call site ([`custom-hooks-v1.md`](../specs/custom-hooks-v1.md)).

## Generated code contract

Ownership, generated base, and regeneration rules are in
[`developer-workflow.md`](developer-workflow.md); the IR contract is in
[`ir-v1.md`](../specs/ir-v1.md). Config keys were designed in [ADR-014](../../decisions/ADR-014-project-config-format.md) (project config format).

- One generated base header per component (`<Component>.g.hpp`) plus the developer-owned
  `<Component>.hpp/.cpp` that derives from it ([ADR-040](../../decisions/ADR-040-generated-base-and-developer-class.md)).
- File header: `// ADR-040 generated base: do not edit.`
- Developer logic lives only in the developer class, in the members its `bind::<id>` markers name;
  a missing member is `TESSERA_CG_MISSING_BINDING` at compile time.
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
everything. The report lists `generated`, `skipped`, `scaffolded`, and `warnings`.

### The generated base (ADR-040)

`tessera_codegen --write` writes `<Component>.g.hpp` (the generated CRTP base, always regenerated,
never edited) and, once, the developer stubs `<Component>.hpp/.cpp` with one member per `bind::` id.
Existing developer files are never touched; a binding they do not define yet is reported as
`TESSERA_CG_MISSING_BINDING` with the stub to add, and a stub marker whose binding left the TSX as
`TESSERA_CG_BINDING_UNUSED`. `--check` reports drift of the generated header only. This is the only
codegen path ([ADR-040](../../decisions/ADR-040-generated-base-and-developer-class.md)); the legacy
protected-region output and its merger are gone (#219).

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
- It never rewrites a developer-owned C++ file; the developer class is written once.
