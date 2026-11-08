# GPU backends and parity

Status: Vulkan/null B1 implemented; WebGPU and software/lavapipe paths proposed. Decisions:
[ADR-001](../../../decisions/ADR-001-vulkan-1-3-baseline.md),
[ADR-002](../../../decisions/ADR-002-webgpu-dawn-for-wasm.md),
[ADR-003](../../../decisions/ADR-003-thin-rhi-and-batched-2d-renderer.md),
[ADR-009](../../../decisions/ADR-009-deterministic-headless-golden-tests.md),
[ADR-010](../../../decisions/ADR-010-macos-via-moltenvk-deferred.md),
[ADR-011](../../../decisions/ADR-011-shader-authoring-and-cross-compilation.md).

## 1. Backend matrix

| Backend | Platform | Status | Purpose |
|---|---|---|---|
| Null | all | B1 done | deterministic command/validation tests |
| Vulkan 1.3 | Windows/Linux/Android | B1 offscreen done | native renderer |
| WebGPU/Dawn | Wasm/designer | proposed | browser preview, shared engine |
| Software/lavapipe | CI | B2 | deterministic goldens/headless |
| MoltenVK | macOS | deferred (ADR-010) | native macOS |
| Metal native | macOS/iOS | not planned | would be a second native API |

No Vulkan inside Wasm. WebGPU is the only Wasm GPU API.

## 2. Capability floor

Every backend must provide:

- one color target with load/store, copy to buffer
- sampled textures with nearest/linear + clamp/repeat
- vertex buffers, triangle lists, alpha blend
- push constants or an equivalent uniform path
- explicit viewport/scissor
- validation/error reporting
- resource creation/destruction with deterministic lifetime

Optional capabilities are queried, not assumed:

- depth/stencil, multiple render targets, compute, timestamp queries, indirect draws
- texture formats beyond `R8G8B8A8_UNORM`/`R8_UNORM`
- bindless/descriptor indexing, multi-draw, geometry shaders

A missing optional capability uses the documented fallback or fails at creation.

## 3. Vulkan 1.3

- Dynamic rendering; no legacy render-pass/framebuffer objects.
- volk loads entry points; VMA sub-allocates memory; one graphics queue in B1.
- B1: one frame in flight, synchronous submit/wait, host-visible buffers.
- Targets: `COLOR_ATTACHMENT | TRANSFER_SRC`; textures: `SAMPLED | TRANSFER_DST` uploaded through
  staging and transitioned to shader-read.
- Readback: copy target to a `TRANSFER_DST` host-random-access buffer, fence-wait, map/read.
- Validation layers opt-in (`--validation`) and mandatory in debug/CI GPU runs.
- B2 adds frames in flight/timeline semaphores, device loss, and persistent pipeline cache.

## 4. WebGPU/Dawn

- Wasm/browser and designer path; native WebGPU may be used for tests if available.
- Bind groups instead of descriptor sets; no push constants; use a uniform buffer.
- WGSL sources are required for shared shaders; reflection/parity checks are build-gated.
- No raw GPU pointers; resource creation goes through the same RHI contract.
- Browser surface/swapchain is owned by the platform/Wasm host.
- Feature detection follows the WebGPU floor; unsupported features use fallbacks.
- Wasm memory/async limits mean uploads are chunked and bounded.

## 5. Null backend

- Records command stats and validates order; no image output.
- Used for fast unit/integration tests on any machine.
- Must implement the same capability queries (with declared limits) so tests catch backend
  assumptions.
- No hidden permissive behavior: invalid sequences fail like a validating backend.

## 6. Software/lavapipe

- Mesa lavapipe is the CI software Vulkan ICD; the CPU rasterizer is the fallback for goldens.
- Golden baselines come only from this path, never a real GPU.
- Fixed offscreen size/DPI/fonts/clock/inputs; AA disabled in `--golden` where required.
- GPU runs are smoke tests with tolerance and never update baselines.
- Software path may be slower; tests use small targets and bounded cases.

## 7. Parity rules

- Same logical resources/bindings/vertex layouts across backends.
- Same blend/color/clip-space conventions; Y-flip is backend-specific and documented.
- Same shader manifest; shared shaders stay inside the WebGPU floor.
- Same golden corpus/structural reports where the path is deterministic.
- Same fallback behavior for missing optional features.
- No backend-specific UI code or raw handles.
- Differences are recorded as capability gaps, not silent behavior changes.

## 8. Device selection and loss

- Backend/device selection is runtime config + HAL policy; the app does not hardcode a device.
- Prefer a discrete/integrated device by documented policy; allow override for tests.
- Device loss: stop submissions, preserve UI state, recreate device/resources, report; if
  recreation fails, show an error surface rather than a crash loop.
- Out-of-memory: evict caches/targets, retry once, then report.
- Backend unavailable: fall back to software/null only where semantics allow (goldens/tests);
  production does not silently render with a different backend.

## 9. Validation and CI

- Vulkan validation layers in debug builds and GPU CI.
- WebGPU validation in browser/designer tests.
- Null contract suite on every CI job.
- Lavapipe golden job for deterministic frames.
- Optional real-GPU smoke job; no baseline updates and no required hardware for PRs.
- Logs include backend, device, driver, format, and feature caps.

## 10. Deferred/open

- Native Metal/other backends.
- MoltenVK production support.
- Android Vulkan device matrix.
- WebGPU native vs browser parity.
- Video decode/external textures.
- Ray tracing/mesh shaders (not V1).
