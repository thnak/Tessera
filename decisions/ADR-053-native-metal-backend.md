# ADR-053: macOS and iOS use a native Metal backend (supersedes ADR-010 items 2, 3 and 5)

- Status: Accepted (2026-11-13, owner: "chúng ta sẽ thêm Metal")
- Date: 2026-11-13
- Related: [ADR-010](ADR-010-macos-via-moltenvk-deferred.md) (superseded in part),
  [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin RHI),
  [ADR-009](ADR-009-deterministic-headless-golden-tests.md) (goldens),
  [ADR-049](ADR-049-backend-extensibility.md) (RHI versioning, shader chain, tiers),
  GitHub issue #188

## Context

ADR-010 deferred macOS and chose MoltenVK (Vulkan translated to Metal) for when macOS is added,
rejecting a native Metal backend only for V1 on cost. Since then ADR-049 made new backends cheap: a
versioned RHI, a `shader_id` registry, and an opt-in build-time path from the single GLSL source to
MSL (`glslc` portable branch, then SPIRV-Cross, then the Metal compiler). A native backend now
costs mostly the device code, not a second shader language. The owner decided to add Metal.

## Decision

1. **A native Metal backend** implements the RHI for macOS (and later iOS) in
   `src/rendering/backend/metal/`, in Objective-C++ confined to that directory. MoltenVK is **not**
   used; the Vulkan backend stays Windows, Linux and Android.
2. **Shaders:** MSL generated at build time through ADR-049's `tessera_cross_compile_shader` path
   (GLSL to SPIR-V to MSL). No runtime shader compilation in production.
3. **Tier:** Tier 2 in-tree ([ADR-049](ADR-049-backend-extensibility.md) section 4) until it passes
   the backend conformance suite on a macOS CI lane; promotion to Tier 1 follows the rules there.
   A hosted runner's GPU may not match hardware, so pixel goldens still come from lavapipe only
   ([ADR-009](ADR-009-deterministic-headless-golden-tests.md)); Metal is checked against
   `tolerance.json`, never used as a golden source.
4. **Platform layer:** Cocoa windowing, input and lifecycle normalized in `src/platform/` and
   `src/hal/` behind `WindowMetrics`, as for the other platforms; NSAccessibility for the a11y tree.
5. **ADR-010 items 2, 3 and 5 are superseded** (MoltenVK, no Metal in V1, evaluating Dawn/Metal).
   Item 1 (macOS is not a V1 *blocker*) and item 4 (clean seams) still stand: macOS is not required
   for the V1 release unless the owner later says so.

Owner confirmation (2026-11-13): native Metal only; MoltenVK is dropped for good, on macOS and iOS.

## Consequences

- Better performance and feature coverage than a translation layer, and no MoltenVK limits
  (geometry shaders, format gaps) to track; cost is a new backend to maintain.
- Needs Mac hardware or a hosted macOS runner for tests; the existing macOS CI lane builds only.
- Prerequisites in the backlog: the backend conformance harness (#161), `Device::capabilities()`
  (#160) and the shader cross-compile path (#159), all `ready` in M3.
- Issue #188 is replaced by the slices below; Dawn/Metal is no longer an alternative.

## Open questions

Not decided; the owner answers them in this section. Details in
[`docs/design/gpu/metal-backend.md`](../docs/design/gpu/metal-backend.md).

1. **Minimum macOS and iOS versions.** Not named. The floor decides whether `MTLSharedEvent`
   `waitUntilSignaledValue:timeoutMS:` is available for the bounded wait that ADR-021 requires on
   the UI thread. Without it the backend needs its own bounded wait. Slice 1 (#204) needs this
   answer first.
2. **Shared or per-backend retire logic.** The retire queue lives inside the Vulkan backend today.
   Preferred: factor it behind a small completion-source interface so Vulkan, WebGPU and Metal share
   one implementation and one set of fake-completion tests. Alternative: Metal reimplements it.
   Slice 1 needs this answer first.
3. **Heaps and untracked resources.** V1 uses default hazard tracking and no `MTLHeap`. If the
   resource cache later wants heaps, explicit `MTLFence` use needs its own design note.

Checks that need a Mac (tracked as a checklist in #204, not decisions): a frame slot and a drawable
cannot deadlock when their counts differ; an errored command buffer always runs its completed
handler; the generated MSL compiles and matches the other backends (ADR-049 notes MSL is
unvalidated without a Metal toolchain).

Infrastructure, not an ADR matter: the macOS CI lane builds only and runs nightly; Tier 2 needs a Mac
or a hosted runner with a GPU, and a hosted runner's GPU may not match hardware.

## Slices (GitHub issues)

1. Metal device, queue, buffers, textures, render targets on the RHI (null-RHI contract tests
   compile on macOS).
2. Pipelines and the MSL shader path; the batched 2D renderer draws a triangle and a rect.
3. Swapchain/`CAMetalLayer` window, resize, present, DPI.
4. Conformance suite pass and the macOS lane runs it; Tier 2 declared.
5. NSAccessibility tree.
