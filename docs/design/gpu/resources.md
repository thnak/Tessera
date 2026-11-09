# GPU resources and memory

Status: B1 resource set implemented; B2/B3 extensions proposed. Asset loading/decoding is in
[`../resources.md`](../resources.md); ownership and budgets are in
[`../../specs/memory-model-v1.md`](../../specs/memory-model-v1.md) and
[`../../specs/buffer-config-v1.md`](../../specs/buffer-config-v1.md).

## 1. Resource classes

| Class | Examples | Owner |
|---|---|---|
| Geometry | vertex/index buffers | renderer/custom element |
| Uniform | push constants, uniform buffers | pipeline pass |
| Staging | upload ring buffers | frame slot |
| Readback | screenshot/test readback | frame slot |
| Sampled | image/glyph/atlas textures | resource cache |
| Attachment | offscreen/swapchain targets | renderer/window |
| Pipeline | shader/fixed state | pipeline cache |

All GPU objects are created through `Device`; no raw backend allocation in UI code.

## 2. Buffers

Rules:

- `BufferDesc` states size and usage; index/vertex/uniform/staging/readback are distinct.
- Vertex stride/attributes are declared in the pipeline; no implicit packing.
- Uniform data uses explicit layout/alignment; push constants are limited by the backend.
- B1 buffers are host-visible (vertex, staging, readback). Device-local buffers are a B2
  optimization, not a semantic requirement.
- Staging buffers are pooled per frame slot; no per-upload allocation in steady state.
- Readback buffers are `TransferDst` with host-random access and a bounded size.
- Buffer writes are explicit and ordered before the draw/copy that uses them.
- Zero-size/overflow/out-of-range writes are errors.

## 3. Textures

B1 formats: `R8G8B8A8_UNORM` and `R8_UNORM`. sRGB/HDR formats require a color-management
decision (not scheduled).

Rules:

- `TextureDesc` states extent, format, filter, address mode, pixels/row pitch, and sample count
  (1 or 4; multisampled textures are storage/attachment-style and cannot be uploaded or sampled by
  the 2D pipeline).
- Upload path: staging buffer -> copy -> transition to shader-read.
- Sampler state is part of the texture desc in B1; no hidden global sampler.
- Mips/compression are opt-in; if unsupported, fail explicitly.
- Texture dimensions/row pitch/decoded size are validated before allocation.
- Atlas textures use deterministic packing/insertion order.
- A texture used by an in-flight frame is not destroyed; retire after completion.

## 4. Render targets

- B1 targets are color-only `R8G8B8A8_UNORM`, `COLOR_ATTACHMENT | TRANSFER_SRC`.
- Load/store ops are explicit (`Clear/Load/DontCare`, `Store/DontCare`).
- MSAA is opt-in per target: `RenderTargetDesc::sample_count` is 1 (default) or 4. A 4x target owns
  a multisample render image (`VK_SAMPLE_COUNT_4_BIT`, `storeOp=DONT_CARE` after resolve) plus a
  1-sample resolve image (`COLOR_ATTACHMENT | TRANSFER_SRC`). The single-sample resolve image is what
  readback and sampling observe; it is the target's persistent content. Unsupported
  format/sample-count combinations fail creation with the requested value (see
  `Device::supports_sample_count`) and never silently fall back to 1x.
- Memory: a 4x target costs roughly `4x` the color-sample bytes for the multisample image plus
  `1x` for the resolve image (about 5x a 1-sample target of the same size); the resolve itself adds
  read/write bandwidth for that target. Targets are cached by size/format/usage/sample count, so 1x
  and 4x variants are distinct cache entries and both can exist.
- Offscreen custom elements may add depth/stencil; format/depth are declared in the element
  pipeline and target.
- Targets are sized in physical pixels; DPI/scale changes recreate them.
- Old targets retire after frames in flight; resize never destroys an acquired target.
- No per-frame target allocation; targets are cached by size/format/usage.

## 5. Samplers and descriptors

- B1 uses one combined image sampler per texture and one descriptor set per texture.
- Bindings are explicit in the shader manifest; reflection mismatch fails the build.
- WebGPU uses bind groups with the same logical binding numbers; push constants map to a uniform
  buffer.
- No hidden global descriptor state; a draw uses the bindings recorded since the last pipeline
  bind.
- Bindless/descriptor indexing is deferred. If added, it needs a capability query and a fallback.
- Descriptor updates are frame-slot safe; a descriptor set is not mutated while in flight.

## 6. Memory and budgets

- VMA sub-allocates Vulkan buffers/images; no raw `vkAllocateMemory` in the backend.
- Categories: vertex, index, uniform, staging, readback, textures, atlases, targets, pipelines.
- Budgets are tracked in bytes and item counts; exceeding a soft budget triggers LRU eviction.
- Staging/readback rings are bounded; a full ring backpressures or defers, never grows.
- Resource caches are bounded and may be disabled with config (`*CacheBudgetMb=0`).
- Out-of-memory: evict caches -> drop optional targets -> retry once -> report. Never crash the
  process or silently render wrong content.

## 7. Lifetime and generations

- Resources have stable ids/generations for diagnostics and hot reload.
- Upload completion is applied on the UI thread at a frame boundary; no callbacks after teardown.
- Hot reload creates a new generation; old generation retires after frames in flight.
- A resource referenced by a submitted frame is not reused for a different asset.
- Shutdown drains/retires caches deterministically, then destroys the device.

## 8. Limits and validation

Validate at creation and before upload:

- nonzero extent/size; row pitch >= width * bytes-per-pixel
- max texture size/dimension and max buffer size
- format support for sampled/attachment/copy usage
- sample count is 1 or 4 and `Device::supports_sample_count(format, count)` is true; a pipeline's
  sample count must equal the target's at draw time
- max push-constant size and descriptor count
- readback/staging budget
- no overflow in `width * height * bpp` or offset + size

Invalid input returns an explicit error; it is not clamped silently.

## 9. Diagnostics

- bytes by category, peak, current, evictions
- cache hit/miss/eviction and generation
- upload/readback bytes and latency
- target/pipeline counts and retired-but-pending counts
- device-lost/OOM/validation events
- per-frame resource creation count (must be zero in steady state)

## 10. Tests

- Null backend: descriptor validation, buffer round-trip, target/copy stats, sample-count
  parity/validation, pipeline/target mismatch rejection.
- MSAA: 4x target create/draw/resolve/readback; resolved image matches a 1-sample render for
  sample-aligned geometry and differs only on non-aligned edges; deterministic repeat.
- Resource cache: eviction, in-flight guard, generation retirement, budget limits.
- Upload: row pitch/format/size validation, staging reuse, ordering before first use.
- Target: load/store, resize recreation, old-target retirement.
- OOM fallback with fake allocator/limit.
- Goldens: texture upload, atlas, target readback.

## 11. Deferred/open

- Device-local vs host-visible buffer policy per backend.
- sRGB/HDR/color-space formats.
- MSAA 8x, per-format sample counts, depth/stencil MSAA, and resolve-policy variants (sample-zero,
  min/max); V1 is color-only 1x/4x with average resolve.
- Bindless descriptors.
- Sparse/virtual textures and residency.
- Compression/transcoding policy.
- Cross-device shared resources.
