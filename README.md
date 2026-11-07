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

_TBD — Phase 0 will add CMake + Vulkan PoC._

## Next

1. ADR-001 … ADR-011 in `decisions/`.
2. Phase 0: Vulkan offscreen quad → PNG, run on CI with Mesa lavapipe.
3. Phase 0: protected-region merger prototype (idempotent).
