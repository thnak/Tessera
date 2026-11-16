# Getting started

Status: the repository is pre-alpha (Big Step B1, the core renderer, is complete; Big Step B2 is in progress; see [AGENTS.md, Big Steps](../../AGENTS.md#big-steps-group-work-dont-micro-step)). The `tessera` CLI and generated-project
workflow are designed but not implemented yet. Today you can build the framework, run its
tests, and run the offscreen/batched2d demos.

## 1. Current repository workflow

### Prerequisites

- CMake >= 3.24
- C++23 compiler with `std::expected` (MSVC 2022/2026, GCC, or Clang; minimum versions are not yet written, see [ADR-038](../../decisions/ADR-038-cpp23-baseline.md))
- Vulkan SDK: headers, loader, `glslc`, validation layers
- `VULKAN_SDK` set (Windows) and `glslc` discoverable
- Ninja (recommended), or a Visual Studio generator
- Optional: `sccache`/`ccache` for faster rebuilds

Linux CI also uses Mesa lavapipe (`mesa-vulkan-drivers`), a software Vulkan ICD (installable client driver, the Vulkan loader's name for a GPU driver), so tests run without a GPU.

### Windows

```powershell
cmake --preset vs2026
cmake --build --preset vs2026-release
ctest --preset vs2026-release
```

If Visual Studio 2026 is unavailable:

```powershell
cmake --preset vs2022
cmake --build --preset vs2022-release
ctest --preset vs2022-release
```

Run the demos:

```powershell
.\build\vs2026\tools\poc\vulkan_offscreen\Release\vulkan_offscreen_poc.exe --out offscreen.png
.\build\vs2026\tools\poc\batched2d\Release\batched2d_poc.exe --backend vulkan --out batched2d.png
.\build\vs2026\tools\poc\batched2d\Release\batched2d_poc.exe --backend null
```

### Linux

```bash
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

Run the demos with lavapipe:

```bash
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/dev/tools/poc/vulkan_offscreen/vulkan_offscreen_poc --out offscreen.png

VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/dev/tools/poc/batched2d/batched2d_poc --backend vulkan --out batched2d.png

./build/dev/tools/poc/batched2d/batched2d_poc --backend null
```

The `null` backend needs no GPU and prints draw/vertex counts; use it for command/batching
experiments.

### Format and lint

```powershell
.\scripts\format.ps1 -Check
.\scripts\lint.ps1 -BuildDir build\dev
```

```bash
bash scripts/format.sh --check
BUILD_DIR=build/dev bash scripts/lint.sh
```

CI pins `clang-format==22.1.3`. Formatting applies to hand-written and generated code, not
vendored `third_party/{stb,volk,vma}`.

## 2. What each demo does

| Demo | What it proves |
|---|---|
| `vulkan_offscreen_poc` | Vulkan 1.3 dynamic rendering, readback, PNG output, no window/swapchain |
| `batched2d_poc --backend vulkan` | RHI + batched 2D (solid/SDF/textured quads, clip/transform stacks) |
| `batched2d_poc --backend null` | Same scene without a GPU; deterministic command stats |
| `tessera_codegen` | Codegen CLI: `.tsx` -> IR -> the ADR-040 generated base plus developer scaffold |

Reference image: [`docs/images/batched2d.png`](../images/batched2d.png).

Useful flags:

```text
batched2d_poc --backend vulkan|null --out file.png --raw file.rgba
             --width N --height N --validation --golden
```

`--golden` disables AA (anti-aliasing) for hard, pixel-stable edges. It is not a full golden-test runner; the pixel-golden pipeline is a Big Step B2 item (see [testing.md](testing.md)).

## 3. Current tests

```bash
ctest --preset dev          # Linux/Ninja
ctest --preset vs2026-release
```

Current tests:

- `codegen_base` — generated-base shape/golden, scaffold, missing/unused binding reports
- `rhi_null` — null device command stats
- `scene_graph` — hit testing, capture/target/bubble, stop propagation
- `batched2d` — batch/vertex counts on the null backend (the RHI backend that records commands without a GPU)
- `batched2d_vulkan_smoke` — Vulkan 1.3 smoke render
- `codegen_cli`

## 4. Troubleshooting

| Symptom | Fix |
|---|---|
| `glslc not found` | Install the Vulkan SDK; set `VULKAN_SDK`; ensure `Bin/glslc` is on `PATH` |
| `find_package(Vulkan)` fails | Install Vulkan headers/loader or point `VULKAN_SDK` at the SDK |
| Vulkan device creation fails | Update the GPU driver; check Vulkan 1.3 + dynamic rendering |
| Crash in CI but not locally | Use lavapipe (`VK_ICD_FILENAMES`) and validation layers |
| Format check fails | Run `scripts/format.*` with the pinned `clang-format` |
| Rebuild is slow | Enable `TESSERA_USE_COMPILER_CACHE=ON` and install `sccache`/`ccache` |
| Wrong preset name | See `CMakePresets.json`: `dev`, `release`, `ci`, `vs2026`, `vs2022` |

## 5. Starting an app (`tessera new`) and the target workflow

`tessera new` ships today (issue #77, first slice). It scaffolds a project and stops there:

```bash
tessera new my-app --template blank     # or --template counter
cd my-app
cmake --preset dev -DTESSERA_SOURCE_DIR=/path/to/Tessera
cmake --build --preset dev
./build/dev/my-app --frames 120
```

The scaffold writes `tessera.json`, `CMakeLists.txt`, `CMakePresets.json`, `.clang-format`,
`.gitignore`, `README.md`, `ui/{App.tsx,app.css,elements.json}` and
`src/{main.cpp,App.hpp,App.cpp}`. It refuses a non-empty directory, is deterministic (two runs with
the same inputs are byte-identical), and **downloads nothing** — the framework has to be an installed
SDK or a checkout you already have, which is why the command above passes `-DTESSERA_SOURCE_DIR`.
No font is scaffolded either: copy any OFL font to `assets/fonts/Inter-Regular.ttf` or pass
`--font <path>`. The generated `README.md` restates all of this.

The rest of the designed workflow is still the target contract, not runnable:

```bash
tessera dev          # design only: Wasm/WebGPU preview in the browser
tessera build        # design only: generate C++ + compile native
tessera run          # design only
```

Today `tessera generate` is the separate `tessera_codegen` binary, and `tessera` itself is the
headless automation harness plus `new`. Independently of this workflow, a Tessera checkout builds
with the presets in section 1.

The project layout, `tessera.json`, hooks subset, and generated-code contract are defined in
[`project-generator.md`](project-generator.md) (project layout and generator) and
[`project-config-v1.md`](../specs/project-config-v1.md) (`tessera.json` schema).

## 6. Next steps

- Read [`docs/README.md`](../README.md) and [`architecture/overview.md`](../architecture/overview.md).
- Read [`developer-workflow.md`](developer-workflow.md) (generated base, developer class and
  regeneration) before editing generated code.
- Read [`testing.md`](testing.md) before adding tests.
- Read [`packaging.md`](packaging.md) before shipping a binary.
