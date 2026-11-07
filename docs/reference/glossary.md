# Glossary

Terms used across Tessera docs. Keep definitions short and link to the normative document.

| Term | Meaning |
|---|---|
| **ADR** | Architecture Decision Record under `decisions/`; append-only. |
| **Backend** | GPU implementation of the RHI: Vulkan, WebGPU, null, software. |
| **Binding** | Stable id (`bind::<id>`) connecting JSX logic to a protected C++ region. |
| **Binding manifest** | Generated JSON mapping binding ids to components/files/regions. |
| **Batching key** | `(scissor, texture)`; consecutive quads with the same key share a draw. |
| **B1/B2/B3** | Big Step B phases: renderer core; runtime slice; window/swapchain/OS. |
| **Command list** | Explicit RHI recording object: render pass, bind, draw, copy, submit. |
| **ComputedStyle** | Typed style after cascade/inheritance/custom properties; consumed by layout/paint/animation. |
| **Dirty domain** | Style/layout/paint/render/resource/GPU invalidation scope. |
| **DPI scale** | Physical pixels per logical pixel; device scale factor. |
| **FoldInfo** | Fold state/orientation/occlusion/separating/hinge bounds for foldables. |
| **Frame pacing** | Scheduling frames to the display/refresh/budget, not free-running. |
| **Golden** | Deterministic reference image + structural report from the software path. |
| **HAL** | Hardware/OS abstraction layer: window, display, input, power, lifecycle, etc. |
| **IR** | Neutral compiler intermediate representation between JSX and C++ codegen. |
| **lavapipe** | Mesa software Vulkan ICD used for deterministic/headless tests. |
| **Logical pixel** | DPI-independent UI unit; physical = logical × scale. |
| **Merger** | Protected-region merge step that preserves developer C++ across regeneration. |
| **Null backend** | No-GPU RHI backend recording command stats for tests. |
| **Protected region** | `[DEVELOPER_LOGIC_START/END: bind::id]` block owned by the developer. |
| **RHI** | Thin render hardware interface; the only GPU seam. |
| **Retained/immediate hybrid** | Retained scene/caches plus per-frame streaming for dynamic data. |
| **Render-on-demand** | Render only on dirty/wake events; static UI idles. |
| **Scene graph** | Retained node tree with bounds/transform/children/events. |
| **Semantics tree** | Accessibility mirror of the scene graph (roles/states/actions). |
| **SPIR-V** | Vulkan shader bytecode; generated from GLSL at build time. |
| **Tessera JSON** | `tessera.json` project configuration (schema v1). |
| **VRR** | Variable refresh rate (adaptive sync); never overrides `maxFps`. |
| **WGSL** | WebGPU shading language. |
| **WindowMetrics** | Normalized window/display/fold metrics consumed by the UI. |
| **Yoga** | Flexbox layout engine used behind the layout adapter. |

## Related

- [`docs/README.md`](../README.md) — documentation map
- [`architecture/overview.md`](../architecture/overview.md) — system overview
- [`architecture/module-map.md`](../architecture/module-map.md) — code map
