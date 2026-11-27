# ADR-072: RHI 1.17, copy a buffer into a texture inside a command list

- Status: **Accepted** for slices 1-2 (2026-11-24, owner decision, with the defaults proposed here:
  fixed 256-byte pitch, Staging sources allowed, two capabilities). Slices 1-2 (the header, the null
  backend, the Vulkan backend, tests) are implemented and the RHI is at 1.17; slices 3-4 (WebGPU,
  Metal) remain proposed.
- Date: 2026-11-24
- Amends: [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) (the RHI surface grows by one
  command and one capability).
- Related: [ADR-071](ADR-071-gpu-compute-tile-rasterizer.md) (the consumer; its spike is why this is
  needed), [ADR-049](ADR-049-backend-extensibility.md) (RHI versioning, conformance, tiers),
  [ADR-068](ADR-068-clip-state-in-a-storage-buffer.md) (storage buffers), issue #683,
  [rhi.md](../docs/design/gpu/rhi.md).
- Terms: a **device-local buffer** is a `BufferUsage::Storage` buffer with `gpu_read_write` (RHI
  1.16): the shader writes it and the host cannot map it. A **texture handoff** is making bytes a
  shader produced into a buffer sampleable as a `Texture` without the CPU touching them.

## Context

The compute rasterizer of ADR-071 writes straight RGBA8 into a device-local storage buffer. The
renderer draws `Texture`s (`PathTextureCache` returns an `rhi::Texture*`, and the batched 2D renderer
binds it with `bind_texture`). Nothing in RHI 1.16 connects the two:

| Way to get compute output into a texture | Why it does not work today |
|---|---|
| `Texture::write` / `write_region` | take a host pointer; the bytes are on the GPU |
| `CommandList::copy_buffer` | buffer to buffer only; the destination is usually a `Readback` buffer |
| read back, then `create_texture(pixels)` | a blocking wait (the UI thread may not take one, CLAUDE.md "never block the UI thread"), or a frame of latency, and a PCIe round trip each way: it keeps the cost the GPU raster exists to remove, and on the web a `queue.writeTexture` of a 4K frame is 29 ms until the GPU has it (ADR-071) |
| `copy_target_region` | render target to render target |

The spike (`tools/poc/compute_raster`) measured what is at stake on an RTX 4060: the 4K dense scene
rasterizes in 4.09 ms of GPU time against 9.2 ms of ThorVG CPU raster plus the upload, and the
output buffer is 31.6 MiB. For icon-sized textures the GPU is **not** faster than ThorVG (64 px: 0.067
ms against 0.022 ms; 256 px: 0.104 against 0.072), and from about 1000 px it is (1024 px: 0.353
against 0.557 ms). So this ADR serves large vector surfaces, animated paths (a transform or a dash
offset is a few bytes per frame, ADR-071), and the web, not the icon cache.

Vulkan already does this operation internally: every texture upload is a `vkCmdCopyBufferToImage`
from a staging buffer (`VulkanDevice::upload_texture_region`), recorded in a separate one-off
submission the command list cannot order against. The new command exposes the same copy inside the
caller's list, ordered after its compute work.

## Decision (proposed)

**RHI 1.17 adds one `CommandList` command and one `Device` capability.**

```cpp
// texture_region.hpp: every backend's row alignment requirement is met by the largest one (WebGPU's).
inline constexpr std::size_t kTextureCopyRowAlignment = 256;
// The smallest valid buffer_row_pitch for a region of `width` texels of `format`.
[[nodiscard]] constexpr std::size_t aligned_texture_copy_pitch(std::uint32_t width, TextureFormat format);

struct BufferTextureCopy {
    TextureRegion region;             // the destination rectangle; an empty region copies nothing
    std::size_t buffer_offset = 0;    // bytes from the start of the buffer to the first texel; a multiple of 4
    std::size_t buffer_row_pitch = 0; // bytes between two rows in the buffer; 0 = aligned_texture_copy_pitch
};

// CommandList
virtual void copy_buffer_to_texture(const Buffer& source, Texture& destination,
                                    const BufferTextureCopy& copy);   // default: throws std::runtime_error
// Device
[[nodiscard]] virtual bool supports_buffer_texture_copy() const { return false; }
```

The contract:

1. **Bytes are copied as they are.** The buffer holds the destination's own byte layout
   (`R8G8B8A8` order for `R8G8B8A8Unorm`/`Srgb`, one byte for `R8Unorm`, `B8G8R8A8` order for the BGRA
   formats); there is no conversion, and an `Srgb` texture decodes on sample as it does today. A
   compute shader writes the channel order and transfer function the texture expects.
2. **Ordering.** The copy is ordered after every dispatch and `copy_buffer` recorded earlier in the
   list; the backend inserts the compute-to-transfer barrier, exactly as `copy_buffer` does now. The
   destination is readable by any draw recorded after the copy in this list and by any list submitted
   later on the device; it is left in the shader-read state (the RHI exposes no image layout, as for
   `bind_sampled_target`). A draw recorded earlier in the same list sees the old contents, because
   the copy cannot sit inside a render pass.
3. **Where it may be recorded.** Outside a render pass (`std::logic_error` inside one), on a texture
   with `sample_count == 1` and `levels == 1` (a mip chain is immutable, as for `write_region`).
4. **Source buffers.** `BufferUsage::Staging` or `BufferUsage::Storage` (device-local or not); any
   other usage is `std::invalid_argument`. A staging source makes the command testable on every
   backend, including those without compute.
5. **Errors, never a silent clip.** A region outside the extent is `std::out_of_range` (as
   `write_region`); a null or foreign buffer or texture, a pitch that is not a multiple of
   `kTextureCopyRowAlignment` or is shorter than the region's row, an offset that is not a multiple
   of 4, a buffer too short for `offset + pitch * (height - 1) + width * bytes-per-pixel`, or a wrong
   usage is `std::invalid_argument`; a lost-device texture (stale epoch) or a backend without the
   capability is `std::runtime_error`.
6. **Lifetime.** The buffer and the texture must outlive the submission, like every other resource;
   the buffer may be rewritten once the list is complete.
7. **A texture created without pixels is uninitialized** until the first copy lands; sampling it
   before then shows undefined texels, as today.

The pitch is fixed at a multiple of 256 bytes on every backend, not "whatever the backend needs":
WebGPU requires `bytesPerRow` to be a multiple of 256 for a multi-row copy, Vulkan's
`bufferRowLength` and Metal's `sourceBytesPerRow` accept it, and one rule means a shader written
for one backend produces a buffer the others accept. The consumer pads its rows; the 4K RGBA8 case
(3840 x 4 = 15360 bytes) needs none.

### Backends

| Backend | Implementation | In 1.17 |
|---|---|---|
| null | validates the whole contract and records the call, so the renderer paths and the validation matrix are unit-testable | answers true |
| Vulkan | in the command list: buffer barrier (shader write to transfer read), image barrier shader-read to transfer-dst, `vkCmdCopyBufferToImage` with `bufferRowLength = pitch / bytes-per-pixel`, image barrier back to shader-read; the same texture state machine `write_region` uses (textures rest in shader-read), the same epoch check | answers true |
| WebGPU (Tier 1) | `wgpuCommandEncoderCopyBufferToTexture`; buffers get `CopySrc`; the pitch rule is its own | answers true since 2026-11-27 with the caveat of slice 3 above; was false until a conformance run could use it (ADR-049 item 3) |
| Metal (Tier 2) | `MTLBlitCommandEncoder copyFromBuffer:...toTexture:`, ending and restarting the encoder around it | answers false; a follow-up |

A backend that answers false throws from the new command and keeps its CPU path; no existing call
changes, so the version moves as a minor (ADR-049 section 2).

### Cost

The copy reads and writes the whole region once. From the device's memory bandwidth (RTX 4060: 272
GB/s) a 4K frame (31.6 MiB read, 31.6 MiB written) is about 0.23 ms, 1024 x 1024 about 0.03 ms and
256 x 256 about 0.002 ms. **These are estimates from the bandwidth, not measurements**; slice 2
measures them with the spike's timestamps. Against the 4.09 ms dense 4K raster that is about 6%.

### Tests and rollout

Each slice is behaviour, tests and docs; the first two land together in one push.

1. **Header, null backend, docs.** `BufferTextureCopy`, `kTextureCopyRowAlignment`,
   `aligned_texture_copy_pitch`, the command, the capability, `version.hpp` 1.17 with its history entry (ADR-049
   names a `CHANGELOG.md`; the tree has none, and 1.1 to 1.16 each recorded themselves in
   `version.hpp`), the `rhi.md` section, and a unit test for the whole
   validation matrix in item 5 plus the ordering record.
2. **Vulkan.** The implementation above. GPU tests under `tests/gpu/rhi/`, on lavapipe: a staging
   buffer copied and sampled back byte for byte; a compute-written storage buffer copied (gated on
   `supports_compute()`, the skip naming it); a sub-rectangle with an offset and a padded pitch; the
   copy ordered against a dispatch before it and a draw after it; the error cases, including inside a
   render pass and a mipmapped destination; a stale device epoch.
3. **WebGPU**, when its conformance lane can run it: implement, run the same contract tests, then
   answer true. Implemented and answering true (2026-11-27, `webgpu_compute.cpp`), but only the path
   rasterizer's use is exercised so far (`webgpu_path_raster_conformance`, Dawn on D3D12: a 4K copy per
   frame, no validation error, ADR-071 "WebGPU numbers"). The contract tests above (sub-rectangle, padded
   pitch, error cases) have not been run on WebGPU.
4. **Metal**, with the Tier-2 maintainer's agreement.

The consumer is **not part of this ADR** and lands separately, after slice 2: the spike's shader
moves into the production shader manifest, the spike's encoder (flatten, stroke records, dashes)
becomes a module of `tessera_path` over `PathShape`, and a `PathTextureCache` path rasterizes on the
GPU when `supports_compute() && supports_buffer_texture_copy()` and the request is large enough to
win (ADR-071 puts the crossover near 1000 px). It records the compute passes and the copy into a list
it owns, `submit_async()`s it, and keeps the work buffers until `is_complete()`; it never waits.
ThorVG stays the reference and the fallback. A tolerance for the GPU against ThorVG goes into
`tests/golden/gpu/tolerance.json`, which ADR-071 leaves undefined.

## Consequences

- The compute rasterizer can feed the renderer without a CPU round trip, which is the only reason the
  web stops paying the 29 ms upload for vector content.
- The command is general: any GPU-produced bytes (a blur or a glyph-outline pass later, ADR-071's
  reuse argument) reach a texture the same way, and a staging buffer reaches one without the
  one-off upload submission.
- The RHI grows by one command and one capability, and two backends (WebGPU, Metal) carry an
  unimplemented default until they are done. A fixed 256-byte pitch costs a padded row when a width
  is small (a 64 px RGBA8 row is 256 bytes and needs none; a 40 px one wastes 96 bytes a row).
- The extra copy is the price of not coupling the texture to compute (see the alternatives).

## Alternatives

| Alternative | Why not (now) |
|---|---|
| **B. Storage-image output**: the compute shader writes the texture directly | saves the copy, but needs a usage flag on `TextureDesc`, restricts formats (Vulkan guarantees `rgba8` storage images; sRGB and BGRA are not guaranteed, WebGPU needs a feature for BGRA), puts the image in the general layout, may disable colour compression on some GPUs, and cannot be mipmapped. A later optimization, not an exclusion |
| **C. Read back and `create_texture`** | a blocking wait or a frame of latency, plus two PCIe trips; defeats the purpose on the web |
| **D. A resolve pass**: a fragment pass reads the storage buffer (RHI 1.12 can bind one) into an offscreen `RenderTarget`, sampled with `bind_sampled_target` (RHI 1.3) | needs **no RHI change** and works on any backend as soon as it has compute. But the result is a `RenderTarget`, not a `Texture`: `PathTextureCache` and the image draw in the batched renderer take a `Texture*`, so the consumers change; each icon costs a render target and a pass; and the path-cache memory category (RHI 1.14) is a texture category. The honest fallback if the owner does not want to touch the RHI |
| **E. A `copy_buffer` overload with a texture destination** | the same API under a name that hides that rows, formats and layouts are involved |

## Open questions for the owner

1. **Is the direction wanted at all?** ADR-071 is still Proposed, and the evidence says the GPU wins
   only for large surfaces. If large vector surfaces are not a V1 need, neither this ADR nor
   ADR-071's consumer is.
2. **Pitch**: one fixed 256-byte rule (proposed) against a per-backend alignment the caller queries.
3. **Staging sources**: allowed here (proposed, so the command is testable everywhere) against
   storage buffers only.
4. **Capability name** and whether the cache should ask one combined capability instead of two.
5. **Alternative D** as the first step, to defer the RHI change until a consumer exists.

## Follow-up

- Slice 1 and 2 above, then the consumer in ADR-071.
- Alternative B once there is a measurement showing the copy matters (it is about 6% of the dense 4K
  raster by the estimate).
