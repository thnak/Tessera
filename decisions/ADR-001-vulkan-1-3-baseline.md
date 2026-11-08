# ADR-001: Vulkan 1.3 as the native rendering baseline

- Status: Accepted
- Date: 2026-11-07
- Related: [ADR-002](ADR-002-webgpu-dawn-for-wasm.md) (WebGPU/Dawn for WebAssembly), [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin render hardware interface and batched 2D renderer), [ADR-009](ADR-009-deterministic-headless-golden-tests.md) (deterministic headless golden tests), [ADR-011](ADR-011-shader-authoring-and-cross-compilation.md) (shader authoring and cross-compilation)
- Evidence: Phase 0 proof-of-concept program (`vulkan_offscreen_poc`) renders a 512x512 image via Vulkan 1.3
  dynamic rendering on Windows/NVIDIA RTX 4060 and passes validation layers.

## Context

Tessera needs a native, high-performance rendering backend for a 2D UI that can sustain
high-frequency chart/animation updates and deterministic offscreen rendering.

Vulkan 1.0/1.1 requires legacy render passes, framebuffers, and verbose synchronization.
Vulkan 1.3 promotes the features that remove most of that boilerplate:

- dynamic rendering (`VK_KHR_dynamic_rendering`, core 1.3)
- synchronization2 (`VK_KHR_synchronization2`, core 1.3)
- timeline semaphores
- copy commands 2
- descriptor indexing is already core since Vulkan 1.2

## Decision

1. **Require Vulkan 1.3** (`VK_API_VERSION_1_3`) for the native backend. Reject older devices
   with a clear error.
2. Enable `VkPhysicalDeviceVulkan13Features::dynamicRendering` and `synchronization2`.
3. **No legacy render passes or framebuffers in new code.** Render targets are `VkImage`s and
   drawing goes through `vkCmdBeginRendering` / `vkCmdEndRendering`.
4. Use **VMA** (VulkanMemoryAllocator) for all allocations from [Big Step B](../AGENTS.md#big-steps-group-work-dont-micro-step) (core renderer) onward. The Phase 0
   PoC (proof of concept) uses raw `vkAllocateMemory` only to keep the proof dependency-free.
5. Load Vulkan through **volk** / `vk-bootstrap` in the real backend; link the loader
   (`Vulkan::Vulkan`) in PoCs.
6. Prefer **timeline semaphores** for cross-queue and [frame pacing](../docs/reference/glossary.md); use fences for one-shot
   submissions.
7. Use **descriptor indexing / bindless** where it materially reduces descriptor churn
   (material and glyph-atlas tables). [V1](../docs/reference/glossary.md) may start with conventional descriptor sets and move
   to bindless once the draw-call batching is stable.
8. **Validation layers are mandatory in Debug builds.** CI (continuous integration) may disable them only when the
   software ICD (installable client driver, the Vulkan driver plug-in) cannot provide the layer.
9. Offscreen color format baseline: `VK_FORMAT_R8G8B8A8_UNORM`. The sRGB/color-management policy
   is not decided in this ADR; it is to be decided in [Big Step B](../AGENTS.md#big-steps-group-work-dont-micro-step) (the Follow-up section says before the first real screen).
10. Platform order: Windows and Linux first; Android after them (mobile work is listed in [Big Step F](../AGENTS.md#big-steps-group-work-dont-micro-step) (production breadth)); macOS only through MoltenVK
    ([ADR-010](ADR-010-macos-via-moltenvk-deferred.md) (macOS via MoltenVK, deferred from the first release)).

## Consequences

### Positive

- Much less boilerplate than Vulkan 1.0/1.1.
- Offscreen-first design directly supports the deterministic golden-test pipeline.
- Modern synchronization and timeline semaphores simplify frame pacing.
- The Phase 0 PoC already exercises the exact feature set (dynamic rendering + offscreen).

### Negative / risks

- Requires relatively modern drivers (roughly 2018+ GPUs; Android devices whose drivers do not report Vulkan 1.3 are rejected by Decision 1).
- Bindless adds complexity and validation edge cases; it is opt-in, not a V1 requirement.
- No native Vulkan on macOS without MoltenVK.
- Software ICD ([lavapipe](../docs/reference/glossary.md)) must support 1.3 for CI; verify on the CI image.

## Alternatives considered

- **Vulkan 1.0/1.1 + extensions.** Rejected: more code, more legacy paths, no benefit.
- **D3D12 / Metal native backends.** Rejected for V1: multiplies backend work; Vulkan covers
  Windows + Linux + Android.
- **OpenGL / OpenGL ES.** Rejected: weaker synchronization, driver variance, poor compute story.
- **Native WebGPU (Dawn) as the primary API.** Rejected: WebGPU is the browser/design-time
  backend ([ADR-002](ADR-002-webgpu-dawn-for-wasm.md)); native Vulkan gives more control and better tooling.

## Follow-up

- Add VMA and volk when the RHI (render hardware interface) lands ([Big Step B](../AGENTS.md#big-steps-group-work-dont-micro-step)).
- Confirm lavapipe reports Vulkan 1.3 + dynamic rendering in CI.
- Revisit sRGB/color-management policy before the first real screen.
