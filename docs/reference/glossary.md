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
| **Bridge** | Boundary that passes data or control to another runtime (JNI, Objective-C, Wasm, IPC, plugin ABI); production bridges are restricted by [ADR-024](../../decisions/ADR-024-native-first-boundaries.md). |
| **Damage** | Screen region that changed and must be redrawn; see [dirty-rendering](../design/dirty-rendering.md). |
| **Epoch** | Counter that identifies the current value or attempt; results carrying an older epoch are discarded. |
| **Error boundary** | Wrapper that catches synchronous exceptions from a component subtree and shows a fallback; see [error-boundaries-v1](../specs/error-boundaries-v1.md). |
| **Frames in flight** | Frames submitted to the GPU that have not finished; resources they use are freed only after they finish. |
| **Glyph atlas** | GPU texture that holds packed glyph bitmaps. |
| **ICD** | Installable client driver: the GPU driver the Vulkan loader selects (for example `VK_ICD_FILENAMES`). |
| **IME** | Input method editor: the platform text-composition layer or on-screen keyboard. |
| **Lease** | Counted hold on a loaded resource; a resource with no leases can be unloaded; see [resource-lifecycle-v1](../specs/resource-lifecycle-v1.md). |
| **SDF** | Signed distance field: shader technique used for rounded-rectangle edges. |
| **Tick** | One fixed-step update of the runtime loop; see [game-loop](../design/game-loop.md). |
| **Tier 1 / Tier 2** | Compiler stages: Tier 1 checks React-style rules (hooks, purity); Tier 2 extracts `bind::` bindings and lowers nodes; see [compiler](../design/compiler.md). |
| **UiId / ComponentId / NodeId** | Numeric identities: UiId and ComponentId are stable per build; NodeId is a runtime instance id and is not stable; see [automation-ids-v1](../specs/automation-ids-v1.md). |
| **V1** | First release scope. [AGENTS.md](../../AGENTS.md), Non-negotiables item 8, limits it to a small, explicit feature set. No document defines which Big Steps V1 includes (open decision). |
| **V1.1** | The first release after V1; no date is set. |
| **VMA** | Vulkan Memory Allocator, the GPU memory library. |
| **volk** | Vulkan function loader library. |
| **G-nn / Q-nn** | Identifiers of gaps and open questions in [app-completeness-cross-check-v1](../architecture/app-completeness-cross-check-v1.md) and [app-completeness-gap-resolutions-v1](../architecture/app-completeness-gap-resolutions-v1.md). |

## Related

- [`docs/README.md`](../README.md) — documentation map
- [`architecture/overview.md`](../architecture/overview.md) — system overview
- [`architecture/module-map.md`](../architecture/module-map.md) — code map
