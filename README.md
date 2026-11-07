# Tessera

> Working name — easy to rename.

A high-performance, cross-platform C++ UI framework:
JSX/React syntax as the design DSL, compiled to native C++,
**Vulkan** as the main rendering backend, **WebGPU (Wasm)** for
zero-emulator design-time preview, and deterministic headless
rendering for AI-agent visual regression.

Status: **pre-alpha / scaffolding**.

## Documents

- [Design plan v1](../ui_framework_design_plan_v1.md)
- [Execution plan & workload breakdown](../ui_framework_execution_plan.md)
- Architecture decisions: [`decisions/`](decisions/)
- Design / specs / RFCs / guides: [`docs/`](docs/)

## Repository layout

```text
Tessera/
├── docs/                 design notes, specs, RFCs, guides, research
├── decisions/            ADRs (ADR-001 ...)
├── include/tessera/      public headers
├── src/
│   ├── core/             object, memory, threading, event, resource
│   ├── hal/              window, display, input, graphics, audio,
│   │                     filesystem, power, timer, lifecycle
│   ├── ui/               view, layout, style, animation, responsive,
│   │                     text, accessibility
│   ├── rendering/        rhi, scenegraph, scheduler, resources, gpu,
│   │                     backend/{vulkan,webgpu,null}, software
│   ├── services/         i18n, navigation, notification, storage, networking
│   ├── diagnostics/      logging, profiling, crash
│   ├── compiler/         analyzer, rules, ir, codegen, merger
│   ├── platform/         windows, linux, macos, android
│   └── runtime/          C++ support runtime for generated code
├── designer/             web (Wasm viewer) + native (IPC host)
├── tests/                unit, integration, golden, fixtures, benchmarks
├── samples/              hello, dashboard
├── tools/                shaders, font, build tooling
├── third_party/          vendored dependencies
├── cmake/                CMake helpers
├── scripts/              dev / CI scripts
└── .github/              CI workflows, issue templates
```

## Build

Requirements: CMake >= 3.24, a C++20 compiler, and the Vulkan SDK
(`VULKAN_SDK` set; `glslc` in the SDK `Bin/` or on `PATH`).

Windows (MSVC 2022):

```powershell
# Visual Studio 2026:
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
# Visual Studio 2022: -G "Visual Studio 17 2022"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
.\build\tools\poc\vulkan_offscreen\Release\vulkan_offscreen_poc.exe --out offscreen.png
```

Linux (headless, Mesa lavapipe):

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.x86_64.json \
  ./build/tools/poc/vulkan_offscreen/vulkan_offscreen_poc --out offscreen.png
```

Phase 0 tools:

- `vulkan_offscreen_poc` — Vulkan 1.3 dynamic rendering into an offscreen image, read back to
  host memory, written as a PNG (no window, no swapchain).
- `merger_poc` — injects developer protected regions into freshly generated C++.

## Next

1. ADR-001 … ADR-011 in `decisions/`.
2. Phase 0: Vulkan offscreen quad → PNG, run on CI with Mesa lavapipe.
3. Phase 0: protected-region merger prototype (idempotent).
