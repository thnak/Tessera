# ADR-001: Vulkan 1.3 as the native rendering baseline

- Status: Accepted
- Date: 2026-11-07
- Related: ADR-002, ADR-003, ADR-009, ADR-011
- Evidence: Phase 0 PoC (`vulkan_offscreen_poc`) renders a 512x512 image via Vulkan 1.3
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
4. Use **VMA** (VulkanMemoryAllocator) for all allocations from Big Step B onward. The Phase 0
   PoC uses raw `vkAllocateMemory` only to keep the proof dependency-free.
5. Load Vulkan through **volk** / `vk-bootstrap` in the real backend; link the loader
   (`Vulkan::Vulkan`) in PoCs.
6. Prefer **timeline semaphores** for cross-queue and frame pacing; use fences for one-shot
   submissions.
7. Use **descriptor indexing / bindless** where it materially reduces descriptor churn
   (material and glyph-atlas tables). V1 may start with conventional descriptor sets and move
   to bindless once the draw-call batching is stable.
8. **Validation layers are mandatory in Debug builds.** CI may disable them only when the
   software ICD cannot provide the layer.
9. Offscreen color format baseline: `VK_FORMAT_R8G8B8A8_UNORM`. sRGB/color-management policy
   is deferred to Big Step B.
10. Platform order: Windows and Linux first; Android later; macOS only through MoltenVK
    (ADR-010).

## Consequences

### Positive

- Much less boilerplate than Vulkan 1.0/1.1.
- Offscreen-first design directly supports the deterministic golden-test pipeline.
- Modern synchronization and timeline semaphores simplify frame pacing.
- The Phase 0 PoC already exercises the exact feature set (dynamic rendering + offscreen).

### Negative / risks

- Requires relatively modern drivers (roughly 2018+ GPUs; some Android devices lag).
- Bindless adds complexity and validation edge cases; it is opt-in, not a V1 requirement.
- No native Vulkan on macOS without MoltenVK.
- Software ICD (lavapipe) must support 1.3 for CI; verify on the CI image.

## Alternatives considered

- **Vulkan 1.0/1.1 + extensions.** Rejected: more code, more legacy paths, no benefit.
- **D3D12 / Metal native backends.** Rejected for V1: multiplies backend work; Vulkan covers
  Windows + Linux + Android.
- **OpenGL / OpenGL ES.** Rejected: weaker synchronization, driver variance, poor compute story.
- **Native WebGPU (Dawn) as the primary API.** Rejected: WebGPU is the browser/design-time
  backend (ADR-002); native Vulkan gives more control and better tooling.

## Follow-up

- Add VMA and volk when the RHI lands (Big Step B).
- Confirm lavapipe reports Vulkan 1.3 + dynamic rendering in CI.
- Revisit sRGB/color-management policy before the first real screen.
