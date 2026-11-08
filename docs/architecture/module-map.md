# Module map

Status: reflects the repository at Big Step B1 plus documented B2/B3 placeholders. Empty
directories marked **planned** are seams, not implemented features.

## 1. Top level

| Path | Responsibility | Status |
|---|---|---|
| `include/tessera/` | Public headers; light, no backend/OS headers | B1 subset |
| `src/core/` | object, memory, threading, events, resources | planned |
| `src/hal/` | window, display, input, lifecycle, power, timer, filesystem, graphics, audio | planned |
| `src/platform/` | Windows/X11/Wayland/Cocoa/Android implementations of HAL | planned (B3) |
| `src/ui/` | view, scene, layout, style, animation, responsive, text, editing, overlay, forms, accessibility | scene B1; rest B2 |
| `src/rendering/rhi/` | thin RHI interfaces | B1 |
| `src/rendering/gpu/` | batched 2D renderer | B1 |
| `src/element/` | custom native element runtime, registry, snapshots | planned (B2/C) |
| `src/rendering/null/` | deterministic no-GPU backend | B1 |
| `src/rendering/backend/vulkan/` | Vulkan 1.3 backend (volk + VMA) | B1 offscreen |
| `src/rendering/backend/webgpu/` | WebGPU/Dawn backend | planned (D) |
| `src/rendering/software/` | deterministic CPU/software path | planned (B2/B3) |
| `src/rendering/resources/` | textures, samplers, uploads, caches | planned |
| `src/rendering/scheduler/` | frame scheduling, fences, presentation | planned (B3) |
| `src/compiler/analyzer/` | parser adapter + React rules | planned (C) |
| `src/compiler/rules/` | framework/binding rules | planned (C) |
| `src/compiler/ir/` | neutral IR + serialization | planned (C) |
| `src/compiler/codegen/` | IR -> C++ | planned (C) |
| `src/compiler/merger/` | protected-region merge | B1 |
| `src/runtime/` | App, loop, generated-code support | planned (B2/C) |
| `src/services/` | i18n, navigation, state restore, storage, networking, notifications, shell | planned (F) |
| `src/data/` | async resource cache, mutations, offline queue | planned (F) |
| `src/diagnostics/` | logging, tracing, profiling, crash reporting | planned (B2) |
| `src/devtools/` | local inspector/hot-reload protocol | planned (C/D) |
| `src/updates/` | signed manifest, staging, rollback | planned (F) |
| `designer/web/` | Wasm + WebGPU viewer, reconciler, IPC | planned (D) |
| `designer/native/` | native host/IPC | planned (D) |
| `tests/unit/` | unit tests (CTest) | B1 subset |
| `tests/integration/` | cross-module tests | planned |
| `tests/golden/` | deterministic goldens + report | planned |
| `tests/fixtures/` | fonts, images, merger/compiler fixtures | B1 merger |
| `tests/benchmarks/` | performance budgets | planned |
| `samples/hello`, `samples/dashboard` | sample projects | placeholder |
| `tools/poc/` | Phase 0/B1 proofs | B1 |
| `tools/shaders/`, `tools/font/` | shader/font tooling | planned |
| `third_party/` | vendored dependencies (stb, volk, vma) | B1 |
| `decisions/` | ADRs | 34 |
| `docs/` | specs, design, guides, architecture | this set |

## 2. Dependency direction

```text
core
 ├─ hal ── platform/{windows,linux,macos,android}
 ├─ ui ── style, layout, animation, text, responsive, accessibility
 ├─ rendering ── rhi ── backend/{vulkan,webgpu,null,software}
 ├─ element ── rhi + shader manifest (custom native elements)
 ├─ data ── services (cache/offline)
 ├─ services
 ├─ diagnostics
 └─ runtime ── ui + rendering + element + hal + services + data + diagnostics

compiler (build time only): analyzer -> rules -> ir -> codegen -> merger
designer (Wasm): runtime/ui/rendering + webgpu + IPC
```

Rules:

- `core` has no OS/GPU/UI dependencies.
- `hal` has no UI/renderer dependencies; platform code implements HAL interfaces.
- `ui` may use RHI *types* (`Rect`, `Mat3`, `Color`) but never backend headers.
- `element` may use the public RHI/shader API; it never includes backend/OS headers.
- `rendering` consumes immutable display lists; it does not mutate the scene graph.
- `runtime` orchestrates; it does not contain compiler/parser code.
- `compiler` is build-time only and must not be linked into production.
- `diagnostics` is cross-cutting but must not pull UI/renderer internals into `core`.

## 3. Public headers

Only `include/tessera/` is public. Current B1 headers:

| Header | Purpose |
|---|---|
| `rendering/rhi/types.hpp` | geometry, color, descriptors, enums, `Mat3` |
| `rendering/rhi/device.hpp` | `Device`, `Buffer`, `Texture`, `Pipeline`, `RenderTarget`, `CommandList` |
| `rendering/gpu/vertex2d.hpp` | `Vertex2D`, attributes, push constants |
| `rendering/gpu/batched2d_renderer.hpp` | batched 2D renderer |
| `element/custom_element.hpp` | custom native element interface/contexts (planned) |
| `rendering/null/null_device.hpp` | null device + command stats |
| `rendering/vulkan/vulkan_device.hpp` | Vulkan device factory (backend-specific) |
| `rendering/vulkan/batched2d_pipeline.hpp` | Vulkan batched2D pipeline factory |
| `ui/scene/node.hpp` | retained scene node, hit testing, dispatch |
| `ui/scene/event.hpp` | event types/phases |

`src/compiler/merger/include/tessera/compiler/merger.hpp` is a build-time public header for the
merger prototype.

## 4. Build targets (current)

| Target | Type | Purpose |
|---|---|---|
| `tessera_rhi` | static | RHI interfaces |
| `tessera_gpu` | static | batched 2D renderer |
| `tessera_rhi_null` | static | null backend |
| `tessera_scene` | static | scene graph/events |
| `tessera_merger` | static | protected-region merger |
| `tessera_vulkan` | static | offscreen Vulkan renderer |
| `tessera_rhi_vulkan` | static | Vulkan RHI backend + batched2D pipeline |
| `tessera_stb` | static | stb image write |
| `vulkan_offscreen_poc` | exe | offscreen Vulkan proof |
| `batched2d_poc` | exe | batched 2D demo (Vulkan/null) |
| `merger_poc` | exe | merger CLI |
| `*_tests` | exe | unit/CLI tests |

## 5. Where to add code

- New UI widget: `src/ui/view/` + `include/tessera/ui/view/`; no backend headers.
- New platform window backend: `src/platform/<os>/` implementing `src/hal/window/`.
- New GPU backend: `src/rendering/backend/<api>/`; add a null fallback/test first.
- New compiler pass: `src/compiler/{analyzer,rules,ir,codegen}/`; IR is the only codegen input.
- New service: `src/services/<name>/`; async, cancellable, no UI blocking.
- New diagnostic sink: `src/diagnostics/<area>/`; bounded and redacted.
- New test: `tests/unit/<area>_tests.cpp` + `tests/unit/CMakeLists.txt`.
- New doc: `docs/{architecture,design,specs,guides,reference}/`; update `docs/README.md`.
- New decision: `decisions/ADR-XXX-slug.md` + `decisions/README.md`.

## 6. Ownership rules

- The compiler owns everything outside protected regions; developers own inside them.
- Generated files are never edited outside `[DEVELOPER_LOGIC_START/END]`.
- The binding manifest is the source of truth for binding ids.
- UI code never owns a window/swapchain; the HAL/runtime does.
- GPU resources are owned by the device; destroy resources before the device.
- Specs are normative; design notes explain; ADRs record contentious choices.

## 7. Naming and size

- Namespaces `tessera::<area>::<module>`.
- One primary type per header; soft 300 / hard 500 lines (ADR-015).
- Public headers stay light; heavy third-party headers in `.cpp`/`detail/`.
- Tests are deterministic and do not read the wall clock, network, or system fonts.
