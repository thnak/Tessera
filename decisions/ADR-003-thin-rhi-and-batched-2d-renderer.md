# ADR-003: Thin RHI plus a batched 2D renderer

- Status: Accepted
- Date: 2026-11-07
- Related: ADR-001, ADR-002, ADR-004, ADR-009

## Context

The framework renders a 2D UI scene: rectangles, rounded rectangles, shadows, text, images,
and vector paths, with clips and transforms. It does not need a general-purpose 3D engine.
The native and Wasm backends (ADR-001, ADR-002) must share as much renderer logic as possible
without leaking API-specific concepts into the UI layer.

Writing a full GPU abstraction is also a trap: too thin and the backends diverge; too thick
and it becomes a second engine.

## Decision

1. Define a **thin RHI** under `src/rendering/rhi/` covering only what the 2D renderer needs:
   - instance / adapter / device / queue
   - buffer and image resources, samplers
   - shader modules and pipeline layouts
   - render pipelines and compute pipelines (compute optional in V1)
   - command recording, render passes via dynamic rendering
   - fences / semaphores / submission
   - bind groups (WebGPU terminology) mapped to descriptor sets on Vulkan
2. Keep the RHI **stateless and explicit**. No hidden global state, no implicit barriers.
   The renderer owns frame graph / barrier policy.
3. Implement a **batched 2D renderer** above the RHI:
   - instanced quad batching for solid and textured rectangles
   - SDF rounded rectangles and borders
   - glyph atlas for text
   - clip stack (rect + rounded-rect; path clips later)
   - transform stack (2D affine)
   - painter's-order sorting; batching keyed by pipeline + texture + clip
4. Use a **retained scene graph + immediate vertex streaming hybrid**:
   - static containers are retained and cached
   - dynamic data (charts, animations) streams vertex data into per-frame buffers
   - dirty flags invalidate only affected nodes
5. **No generic 3D pipeline, no scene lighting, no PBR** in V1. Add only if a concrete UI
   feature requires it.
6. Vector paths are delegated (ADR-004); text is delegated (ADR-006).

## Consequences

### Positive

- Renderer logic is shared between Vulkan and WebGPU.
- The rendering model matches the UI workload (2D, batching, retained/immediate hybrid).
- Dirty-flag invalidation keeps high-frequency chart updates cheap.

### Negative / risks

- Batching correctness (clip changes, texture changes, blend modes) is subtle; needs visual
  regression tests.
- An RHI that is too thin leaks backend details; too thick becomes a second engine. Keep the
  interface to the list above and resist growth.
- Retained/immediate hybrid needs careful lifetime rules for buffers and descriptors.

## Alternatives considered

- **Adopt bgfx.** Rejected: multi-backend and mature, but a large opinionated engine; WebGPU
  support is not the primary path and the abstraction is broader than we need.
- **Use Skia for everything (Vulkan + software).** Rejected as the sole renderer: Skia is
  excellent for paths/text, but it does not give us the scene graph, game clock, or the
  WebGPU design-time path.
- **Immediate-mode only (Dear ImGui style).** Rejected: retained containers and targeted
  invalidation are required for large dashboards.
- **Full 3D renderer.** Rejected: out of scope; costs dominate the UI use case.

## Follow-up

- Write the RHI interface and a null backend test before the Vulkan implementation.
- Define the batching key and sort order precisely in `docs/design/renderer.md`.
- Add a frame-budget/dirty-flag design note in Big Step B.
