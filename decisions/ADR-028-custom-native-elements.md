# ADR-028: Custom native elements, low-level rendering, and element input

- Status: Accepted
- Date: 2026-11-07
- Related: [ADR-001](ADR-001-vulkan-1-3-baseline.md) (Vulkan 1.3 baseline), [ADR-007](ADR-007-compiler-ir.md) (compiler intermediate representation), [ADR-011](ADR-011-shader-authoring-and-cross-compilation.md) (shader authoring and cross-compilation), [ADR-024](ADR-024-native-first-boundaries.md) (native-first boundaries), [ADR-027](ADR-027-agent-automation-and-audits.md) (agent automation and audits)

## Context

Declarative components cannot express custom shaders, high-performance 3D, or element-specific
picking. The framework needs an escape hatch that stays native-first, deterministic, testable,
and reusable, without a JavaScript runtime, dynamic plugins, or a second UI model.

## Decision

1. **Declaration:** custom elements are declared in JSX as
   `<CustomElement element="Waterfall3D" ... />` and in `ui/elements.json`; `element` is a
   static literal. The compiler generates a compile-time registry (`elements.g.cpp`) and typed
   JSX definitions (`elements.d.ts`). No dynamic component types or runtime loading.
2. **Native implementation:** each element is a C++ class deriving
   `tessera::element::CustomElement`, with `on_attach/on_update/on_resize/on_render/on_event/
   on_semantics/on_detach`, plus snapshot/test hooks.
3. **Low-level API:** elements may use the public RHI (render hardware interface) (`Device`, `CommandList`, `Pipeline`,
   `Buffer`, `Texture`, `RenderTarget`, `BindGroup`) and frame arenas/resource caches. No raw
   Vulkan/WebGPU/OS handles in reusable elements; no runtime shader compilation.
4. **Shaders:** referenced from `ui/elements.json`; declared/compiled/reflected via the shader
   manifest; pipeline state (topology/depth/cull/blend/formats) is explicit.
5. **3D composition:** `offscreen` is the default (isolated target, composited as a texture);
   `inline` is an advanced opaque/full-bleed path with a depth attachment and explicit ordering
   constraints. `onDemand` is default; `continuous` animates while visible.
6. **Input:** elements receive normalized events through the scene-graph capture/target/bubble
   pipeline with element-local coordinates; they may refine hit testing (`part_id`), capture
   pointers, request focus, stop propagation, and emit typed custom events.
7. **Reuse:** multiple instances, keys, props/state, deterministic lifecycle; no singletons,
   static mutable state, or global init order. Shared pipelines/meshes/textures use the resource
   cache; resources retire after frames in flight.
8. **Testing:** unit tests with a fake context; structural headless with null RHI; [golden](../docs/reference/glossary.md) with
   software/[lavapipe](../docs/reference/glossary.md); GPU smoke on real devices. Test hooks are debug/headless-only. Snapshots
   expose element state, draw stats, and last event; traces carry numeric IDs.
9. **Security/failure:** custom elements are trusted compile-time native code. Manifests are
   validated; shaders are build-controlled; render errors return `Error` (no throw across
   threads/ABI); update/input exceptions are caught by error boundaries; device loss is handled.

## Consequences

### Positive

- Custom shaders/3D are first-class without leaving the component model.
- JSX remains the declaration surface; TypeScript types come from the manifest.
- Input/picking and tests use the same paths as built-in nodes.
- Reuse/lifecycle/determinism align with normal components.
- No JS runtime, dynamic plugin ABI (application binary interface), or runtime shader compilation.

### Negative / risks

- The RHI escape hatch can be misused; code review and API limits are required.
- Offscreen composition costs memory/copy; inline has ordering constraints.
- Shader/3D portability (Vulkan vs WebGPU) must be explicit.
- Custom elements can hide state/accessibility; snapshots and semantics are mandatory.
- Test hooks and GPU resources add maintenance surface.

## Alternatives considered

- **WebView/canvas/JS runtime.** Rejected: bridge, per-frame cost, security, non-native.
- **Dynamic plugins / `dlopen`.** Rejected: ABI/security/determinism.
- **Only declarative nodes.** Rejected: cannot express custom shaders/3D.
- **Raw Vulkan/WebGPU in elements.** Rejected: portability, safety, backend leakage.
- **Immediate/continuous rendering only.** Rejected: power/determinism; `onDemand` default.
- **Coordinates for input/automation.** Rejected: brittle; use node/part IDs.

## Follow-up

- Define the exact public `element` headers and RHI command surface.
- Add manifest validation, generated registry/typings, and deterministic IDs.
- Implement offscreen/inline composition and target retirement.
- Add input part IDs/custom event bindings and semantics.
- Add unit/structural/golden/GPU-smoke fixtures and trace spans.
- Decide indirect/compute/MSAA/raw-handle scope and WebGPU parity.
