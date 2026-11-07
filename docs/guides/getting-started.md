# Getting started

Status: the repository is pre-alpha (Big Step B1). The `tessera` CLI and generated-project
workflow are designed but not implemented yet. Today you can build the framework, run its
tests, and run the offscreen/batched2d demos.

## 1. Current repository workflow

### Prerequisites

- CMake >= 3.24
- C++20 compiler (MSVC 2022/2026, GCC, or Clang)
- Vulkan SDK: headers, loader, `glslc`, validation layers
- `VULKAN_SDK` set (Windows) and `glslc` discoverable
- Ninja (recommended), or a Visual Studio generator
- Optional: `sccache`/`ccache` for faster rebuilds

Linux CI also uses Mesa lavapipe (`mesa-vulkan-drivers`) for a software Vulkan ICD.

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
| `merger_poc` | Protected-region merge/idempotency prototype |

Reference image: [`docs/images/batched2d.png`](../images/batched2d.png).

Useful flags:

```text
batched2d_poc --backend vulkan|null --out file.png --raw file.rgba
             --width N --height N --validation --golden
```

`--golden` disables AA for hard, pixel-stable edges. It is not yet a full golden-test runner.

## 3. Current tests

```bash
ctest --preset dev          # Linux/Ninja
ctest --preset vs2026-release
```

Current tests:

- `merger_unit` — protected-region extract/merge/idempotency
- `rhi_null` — null device command stats
- `scene_graph` — hit testing, capture/target/bubble, stop propagation
- `batched2d` — batch/vertex counts on the null backend
- `batched2d_vulkan_smoke` — Vulkan 1.3 smoke render
- `merger_cli_help`, `merger_cli_idempotent`

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

## 5. Target application workflow (not implemented yet)

The designed workflow is:

```bash
tessera new my-app --template dashboard --targets windows,linux
cd my-app
tessera dev          # Wasm/WebGPU preview
tessera build        # generate C++ + compile native
tessera run
```

The project layout, `tessera.json`, hooks subset, and generated-code contract are defined in
[`project-generator.md`](project-generator.md) and
[`project-config-v1.md`](../specs/project-config-v1.md). Treat this as the target contract, not
a currently runnable CLI.

## 6. Next steps

- Read [`docs/README.md`](../README.md) and [`architecture/overview.md`](../architecture/overview.md).
- Read [`developer-workflow.md`](developer-workflow.md) before editing generated code.
- Read [`testing.md`](testing.md) before adding tests.
- Read [`packaging.md`](packaging.md) before shipping a binary.
