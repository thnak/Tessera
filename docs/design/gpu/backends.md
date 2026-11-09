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
- texture formats beyond the 8-bit set (`R8G8B8A8`/`B8G8R8A8` UNORM and their sRGB siblings,
  `R8_UNORM`)
- bindless/descriptor indexing, multi-draw, geometry shaders

A missing optional capability uses the documented fallback or fails at creation.

## 3. Vulkan 1.3

- Dynamic rendering; no legacy render-pass/framebuffer objects.
- volk loads entry points; VMA sub-allocates memory; one graphics queue in B1.
- Host-visible buffers in B1. Submission is asynchronous (`submit_async`); each list owns a
  command pool, buffer, and fence taken from a small recycled free list (capped at 8). Lists that
  were dropped while running are reaped by polling their fences, never by waiting.
- Texture upload does not block: staging buffers stay alive in a pending list until the upload
  completes, and destroying a texture defers the Vulkan objects until every upload that could
  reference them has finished (a monotonically increasing upload serial).
- Descriptor sets come from a pool created with `FREE_DESCRIPTOR_SET`; destroying a texture frees
  its set, so create/destroy churn does not exhaust the pool.
- Swapchain present waits on a per-image render-finished semaphore signaled by `submit_async`, so
  the CPU does not wait for the GPU between render and present.
- With `validation` on, synchronization validation is enabled whenever the layer supports it.
- Targets: `COLOR_ATTACHMENT | TRANSFER_SRC`; textures: `SAMPLED | TRANSFER_DST` uploaded through
  staging and transitioned to shader-read.
- Readback: copy target to a `TRANSFER_DST` host-random-access buffer, fence-wait, map/read.
- Validation layers opt-in (`--validation`) and mandatory in debug/CI GPU runs.
- B2 adds timeline semaphores, device loss, and persistent pipeline cache. Binary fences and
  per-image semaphores are what exist today.

### Color formats and sRGB (D2)

The RHI exposes one 8-bit color family: `R8G8B8A8Unorm`, `B8G8R8A8Unorm`, and their
`R8G8B8A8Srgb`/`B8G8R8A8Srgb` siblings (plus `R8Unorm` for coverage/masks). The sRGB variants map to
`VK_FORMAT_*_SRGB` for textures, render targets, pipeline color formats, and swapchain images;
`is_srgb()` in `rhi/types.hpp` identifies them. Their byte layout is identical to the UNORM
siblings, so a readback of an sRGB target returns sRGB-encoded 8-bit channels in the format's byte
order (`RGBA` or `BGRA`).

Format policy:

- **Offscreen/golden path stays UNORM.** A `RenderTargetDesc`/`TextureDesc` defaults to
  `R8G8B8A8Unorm`; the deterministic offscreen renderer and `--golden` keep that default, so the
  lavapipe/CPU golden bytes and the windowed smoke baselines are unchanged. A caller opts into sRGB
  explicitly by setting `format`.
- **Swapchain prefers sRGB.** Surface format selection (`src/rendering/backend/vulkan/src/
  vulkan_surface_format.hpp`, unit-tested with plain data) picks, in order:
  1. `R8G8B8A8Srgb`, then `B8G8R8A8Srgb`, advertised with `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR`;
  2. `R8G8B8A8Unorm`, then `B8G8R8A8Unorm`, with an sRGB non-linear color space (the pre-D2
     behavior);
  3. any of those four formats with any color space, so an unusual color-space pairing still gets a
     swapchain;
  4. a lone `VK_FORMAT_UNDEFINED` entry ("any format"): `R8G8B8A8Srgb` for an sRGB color space,
     `R8G8B8A8Unorm` otherwise.
  The chosen format is exposed through `Swapchain::format()`; presenters that build a pipeline must
  set `PipelineDesc::color_format` from it (`batched2d_pipeline_desc()` returns UNORM by default, so
  callers overwrite the field with `swapchain->format()`/`target->format()`).
- **Approximation.** Rendering into an `*Srgb` target encodes the fragment output from linear light
  to sRGB on write (and hardware sampling decodes sRGB textures); the shader/vertex colors are the
  authored 8-bit values passed through as normalized floats without a color-space conversion, so an
  sRGB target is not byte-identical to the UNORM path. Blending also follows the hardware's color
  space rules rather than the encoded values an author might expect. V1 accepts this documented
  approximation; linear-light rendering, HDR output, and per-surface color management stay out of
  scope.
- **Capability checks.** `create_render_target` validates color-attachment/transfer support via
  `vkGetPhysicalDeviceFormatProperties` and throws for an unsupported format; tests skip such a
  device cleanly rather than requiring sRGB everywhere.

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
- Accepts every 8-bit color format the Vulkan backend accepts, including the sRGB variants, and
  records the declared format on textures/targets; it does not emulate the sRGB encode.
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
- Same color-format policy: offscreen defaults to UNORM, presentation prefers sRGB, and a pipeline's
  color format always matches its target.
- Same shader manifest; shared shaders stay inside the WebGPU floor.
- Same golden corpus/structural reports where the path is deterministic.
- Same fallback behavior for missing optional features.
- No backend-specific UI code or raw handles.
- Differences are recorded as capability gaps, not silent behavior changes.

## 8. Device selection and loss

- Backend/device selection is runtime config + HAL policy; the app does not hardcode a device.
- Prefer a discrete/integrated device by documented policy; allow override for tests.

### 8.1 Device loss (D7)

Loss is recoverable backend state, not an exception or a silent success:

- **Detection.** The Vulkan backend records `VK_ERROR_DEVICE_LOST` from `vkQueueSubmit` (frame and
  upload submits), `vkWaitForFences`/`vkGetFenceStatus`, `vkAcquireNextImageKHR`,
  `vkQueuePresentKHR`, and `vkDeviceWaitIdle`. It bumps the device generation and sets
  `Device::status() == Lost`; the detection path never throws. `Swapchain::acquire_next_image` /
  `present` return the new `SwapchainError::DeviceLost`, a command list's `wait()`/`is_complete()`
  report incomplete, and `submit()` returns without touching the dead device. Waits stay bounded:
  acquire/present are clamped to 100 ms, the synchronous offscreen `submit()` is capped at 60 s, and
  async frame submission never blocks (ADR-021).
- **Lifecycle.** On `Lost` the caller stops submitting/presenting, releases the swapchain and every
  RHI object created before the loss, then calls `Device::recover()` once. Recovery drops
  backend-owned bookkeeping (command-resource pools, pending uploads, descriptor pool/layout, VMA
  allocator), recreates the `VkDevice` + graphics queue + allocator + descriptor state, and re-picks
  the physical device (the `VkInstance` survives). It returns
  `std::expected<void, DeviceRecoveryError>` and never throws:
  `NotSupported` (null/software backends), `NotLost`, `ResourcesOutstanding` (destroy the wrappers
  first; this does not consume the attempt), `DeviceCreationFailed` (device stays `Lost`), or
  `AlreadyAttempted` after a failed one-shot recovery (a new loss is needed).
- **What dies vs what the backend keeps.** Every `VkDevice` child dies with the loss: buffers,
  textures, pipelines, offscreen render targets, command lists, and the swapchain's images, views,
  semaphores, and fences. Callers must recreate those and a new swapchain after recovery. The
  backend keeps only device-independent state: the `VkInstance` and debug messenger, the
  `DeviceDesc`/selection policy, and the internal generation counter. Resources from the lost
  generation are generation-guarded: a late destructor abandons its handles instead of calling the
  dead device (safe, no double destroy); a real loss's VMA allocator state is abandoned with the
  device rather than touched.
- **Test seam.** `TESSERA_VULKAN_TEST_HOOKS` (CMake option, defaults to `TESSERA_BUILD_TESTS`)
  exposes internal fault injection on the Vulkan device: report loss from the next submit/fence
  wait/acquire/present, simulate a loss while keeping the real device alive, or fail the next
  recovery. Tests in `tests/gpu/vulkan/vulkan_device_loss_tests.cpp` and the Win32 swapchain test
  exercise detection, invalidation, one-shot recovery, rendering after recreation, and the failure
  path without a GPU reset; validation layers stay enabled on that path.
- **Remaining.** Real GPU-reset behaviour across drivers/vendors, per-platform UX policy (error
  surface, retry cadence, telemetry), multi-device/multi-swapchain orchestration, surface
  recreation policy, loss detection from allocation failures, and wiring recovery into the
  `RuntimeLoop` as a time-budgeted task on a later tick.
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
- Linear-light rendering, HDR/wide-gamut output, and per-surface color management (D2 keeps V1 to
  8-bit sRGB-encoded formats and documents the resulting blend/encode approximation).
