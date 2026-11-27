# RHI contract

Status: B1 implemented; B2/B3 extensions proposed. This document defines the contract expected
from every backend. [`../renderer.md`](../renderer.md) records the B1 implementation.

## 1. Role

The RHI is a thin, explicit seam. It exposes opaque GPU objects and command recording; it does
not hide synchronization, memory, or backend policy. It is not a scene graph or a render graph.

## 2. Object model

| Object | Owns | Created by | Destroyed by |
|---|---|---|---|
| `Device` | backend instance/queues/caches | backend factory | owner of the device |
| `Buffer` | GPU buffer + memory | `Device::create_buffer` | owner, before device |
| `Texture` | image + sampler metadata | `Device::create_texture` | owner, before device |
| `RenderTarget` | attachment(s) | `Device::create_render_target` | owner, before device |
| `Pipeline` | shader/fixed state | `Device::create_pipeline` | owner, before device |
| `CommandList` | recorded commands | `Device::create_command_list` | owner, after submit/reset |

Handles are opaque. No public header may include Vulkan, volk, VMA, Dawn, or WebGPU headers.

## 3. Device

Current B1:

```cpp
DeviceDesc { Backend backend; bool validation; int device_index; SurfacePlatform surface; }
Device::backend();
Device::wait_idle();
```

RHI versioning and capabilities (ADR-049):

- `include/tessera/rendering/rhi/version.hpp` defines the published RHI contract version as
  `kRhiVersionMajor`, `kRhiVersionMinor`, and `kRhiVersion = (major << 16) | minor`
  (`1.0` / `0x00010000` for the first published version). The current contract is **1.20**
  (`0x00010013`): 1.0 is the B1 surface; 1.1 adds `CommandList::copy_target_region` for the
  isolated-layer seam (issue #242, [`offscreen-layers.md`](offscreen-layers.md)); 1.2 adds the
  swapchain composite-alpha request and the resolved-mode query (glass-v1.md section 6, issue #243);
  1.3 adds the sampled view of an offscreen render target (`CommandList::bind_sampled_target` plus
  the target's sampler state, issue #254, section 5.1 below); 1.4 adds
  `DeviceDesc::surface`/`SurfacePlatform`, the presentation surface family a device is created for
  (issue #155, Wayland window + `VK_KHR_wayland_surface`); 1.5 adds `BlendMode::DualSource` and the
  capability that gates it, `Device::supports_dual_source_blend()` (issue #471, section 4.1 below);
  1.6 adds `TextureDesc::levels`, `Texture::levels()` and the capability that gates it,
  `Device::supports_texture_mipmaps(format)` (issue #491, section 5.2 below); 1.7 adds
  `CommandList::bind_texture_array`, `PipelineDesc::texture_array`, `VertexFormat::UInt` and the
  capability that gates them, `Device::texture_array_capacity()` (issue #480 part 2, section 5.3
  below); 1.8 adds `Backend::Metal`, the native macOS/iOS backend
  ([ADR-053](../../../decisions/ADR-053-native-metal-backend.md),
  [`metal-backend.md`](metal-backend.md)). No enumerator changes value and no interface member
  changes, so a backend that does not implement Metal is unaffected; 1.9 adds
  `Texture::write_region` and `TextureRegion` (issue #681, section 5.4 below); 1.10 adds
  `SurfacePlatform::Android`, the `VK_KHR_android_surface` presentation family (issue #189). No
  enumerator changes value and no interface member changes, so a backend that never presents to an
  `ANativeWindow` is unaffected; the enumerator exists so an Android build can name its family
  instead of resolving to `NoPlatform`; 1.11 gives every RHI `enum class` an explicit
  `std::uint8_t` base type (issue #734). No enumerator changes value and no member is added or
  removed, so every caller compiles unchanged, but `sizeof` shrinks (`RenderTargetDesc` 48 -> 40,
  `DeviceDesc` 16 -> 12), which is an ABI change rather than a source one - hence a minor bump;
  1.12 adds `BufferUsage::Storage`, `CommandList::bind_storage_buffer`,
  `PipelineDesc::storage_buffer` and the capability that gates them,
  `Device::supports_storage_buffers()` (ADR-068 slice 1, issue #669, section 5.5 below). The
  enumerator is appended so no existing value changes, and the capability is pure virtual so every
  backend answers; a backend answering false keeps the push-constant clip path and no frame changes;
  1.13 states the color-space contract for `TextureFormat`/`Swapchain::format()` and moves
  presentation to UNORM (issue #791, section 4 below). No enumerator or member changes and every
  caller compiles unchanged; the surface format a backend chooses does change, which the minor
  marks (see the color formats note in section 4);
  1.14 adds `MemoryCategory`, `MemoryCategoryStats`, `TextureDesc::category` and
  `DeviceMemoryStats::categories` (issue #703, section 5.6 below). memory-model-v1 section 4 asks for
  explicit per-category GPU budgets, and a budget needs a category to attach to: the allocator's
  totals say how much the device holds, not what it holds it for. The new fields are appended with
  defaults (`Other`, all-zero), so every existing caller compiles and behaves unchanged and a
  backend that does not fill the categories reports zeros rather than a wrong attribution. The minor
  moves because `DeviceMemoryStats` grew, which is an ABI change for anything that copies it.
  1.15 adds `Pipeline::declares_storage_buffer()`, so a caller can ask whether the pipeline it is
  about to draw with reads a storage buffer before binding one (ADR-068 slice 3, issue #671). The
  default is false, which is the answer for every pipeline created without
  `PipelineDesc::storage_buffer`, so no existing backend or caller changes.
  1.16 adds compute (issue #683, [ADR-071](../../../decisions/ADR-071-gpu-compute-tile-rasterizer.md)):
  `ComputePipelineDesc`/`ComputePipeline`, `BufferDesc::gpu_read_write` (a device-memory storage buffer
  shaders write), the `CommandList` calls `bind_compute_pipeline`, `bind_compute_buffer`,
  `compute_push_constants`, `dispatch`, `compute_barrier` and `copy_buffer`, and the capability that gates
  them, `Device::supports_compute()`. Every new virtual has a default (false / throws), so no existing
  backend or caller changes; Vulkan implements it, the null and WebGPU backends answer false.
  1.17 adds `CommandList::copy_buffer_to_texture` and `Device::supports_buffer_texture_copy()`
  ([ADR-072](../../../decisions/ADR-072-rhi-copy-buffer-to-texture.md), section 5.7); Vulkan and the null
  backend answer true, WebGPU and Metal false until their slices land.
  1.18 adds `ComputeCapabilities`, `Device::compute_capabilities()` and `CommandList::dispatch_indirect`
  ([`compute.md`](compute.md), issue #684); Vulkan implements them, and the null backend records compute
  only when made with `create_null_compute_device`. WebGPU and Metal keep the defaults (zeroed capabilities,
  throwing calls) until their slices land.
  1.19 adds `BlendMode::DestinationIn` (`dst * src.a` on all four channels), the mask step of an isolated
  layer (issue #617, section 4.1); every backend expresses it with fixed blend factors.
  1.20 adds `Device::supports_analytic_gradients()` (non-pure, default false; true on Vulkan with storage
  buffers, null and WebGPU): the renderer draws a gradient as one quad evaluated per fragment, else bands
  ([gradients.md](gradients.md), ADR-076).
  All later versions are additive compatible changes.
- `RhiCapabilities { Backend backend; std::uint32_t rhi_version; GpuTimestampCapability timestamps; }`
  is reported by `Device::capabilities()` (default: current backend, `kRhiVersion`,
  `timestamp_capability()`). Backends declare the implemented major version with a compile-time
  `static_assert` against `kRhiVersionMajor` and report it via capabilities for runtime/conformance
  logging. The type is not named `DeviceCapabilities` because `<wingdi.h>` defines that word as a
  macro; the public RHI headers stay includable beside Windows headers.
- A PR that changes `include/tessera/rendering/rhi/**` must update `version.hpp` and this contract
  doc to state the version impact; the docs gate fails RHI header changes without those matching
  updates. The version itself is checked by `scripts/check_version_policy.py` (gate
  `version-policy`, issue #194, policy in
  [`versioning-v1.md`](../../specs/versioning-v1.md) section 11.2): the gate fails a change to these
  headers that leaves `kRhiVersionMinor`/`kRhiVersionMajor` where it was. The docs rule catches a
  forgotten companion file; the policy gate catches a forgotten version.

The floor-versus-capability model and the rule for where a new feature goes are in
[`backends.md`](backends.md) section 2.1.

Target additions:

- `DeviceCaps`: format support, max texture size, frames-in-flight limit, compute, push-constant
  limit, validation state (backend/RHI version/timestamps are reported by `RhiCapabilities`).
- `DeviceStatus`: ready, device-lost, out-of-memory, unsupported.
- `Device::wait_idle(timeout)` is for teardown/debug only, never per frame.
- One graphics queue in B1; queue selection is backend policy. Compute/transfer queues are
  deferred until a concrete need exists.

## 4. Descriptors

B1 fields:

| Desc | Fields |
|---|---|
| `BufferDesc` | `size`, `usage` (`Vertex/Index/Uniform/Staging/Readback`) |
| `TextureDesc` | `extent`, `format`, `pixels`, `row_pitch`, `filter`, `address` |
| `RenderTargetDesc` | `extent`, `format`, `clear_color`, `load_op`, `store_op`, `sample_count`, `filter`, `address` |
| `PipelineDesc` | vertex/fragment SPIR-V, vertex layout, color format, blend, push-constant size |
| `SwapchainDesc` | `extent`, `format`, `image_count`, `vsync`, `native_window`, `native_display`, `per_pixel_alpha` |

Presentation surface family (RHI 1.4, additive):

- `DeviceDesc::surface` selects the window system the device will present to: `Auto` (default),
  `NoPlatform`, `Win32`, `Xlib` or `Wayland`. `Auto` is resolved by the backend at creation from the
  platform it was built for and the process environment, so existing callers are source- and
  behaviour-compatible; a caller that holds a HAL window passes the matching value instead of
  relying on the guess. The "no presentation" value is `NoPlatform`, not `None`, because
  `<X11/Xlib.h>` defines `None` as a macro and the public RHI header stays includable beside Xlib.
- The Vulkan backend enables `VK_KHR_surface` plus the surface extension of the resolved family. A
  Linux build may have both Xlib and Wayland support compiled in: all *available* surface extensions
  of the compiled-in families are enabled on the instance (an unused instance extension is harmless
  and it removes any ordering dependency between window creation and device creation), while queue
  presentation support and `create_swapchain` use only the resolved family.
- `SwapchainDesc::native_window`/`native_display` keep the documented per-family meaning; they do
  not identify the family (see `swapchain.hpp`).

Presentation notes (RHI 1.2, additive):

- `SwapchainDesc::per_pixel_alpha` asks for a composite mode that lets a translucent window's client
  area reach the compositor (docs/specs/glass-v1.md section 6). It is a request, not a guarantee.
- `Swapchain::alpha_mode()` reports the mode actually obtained (`Opaque` or `PreMultiplied`) and
  `supports_per_pixel_alpha()` reports whether the surface offers one; a caller applies its opaque
  fallback on the first without failing the frame.
- The old opaque selection is unchanged when `per_pixel_alpha` is false, so every existing caller
  keeps byte-identical behaviour.

Color formats (D2, corrected by issue #791):

- `TextureFormat` carries the 8-bit color family: `R8G8B8A8Unorm`, `B8G8R8A8Unorm`, their
  `R8G8B8A8Srgb`/`B8G8R8A8Srgb` siblings, plus `R8Unorm` for coverage/masks. `is_srgb()` identifies
  the sRGB-encoded variants; their byte layout matches the UNORM sibling and only the transfer
  function differs.
- A `*Srgb` attachment encodes fragment output from linear light on write and a `*Srgb` texture
  decodes on sample. That is for a path that really holds linear light or authored sRGB bytes.
- A `RenderTargetDesc`/`TextureDesc` defaults to `R8G8B8A8Unorm`, and presentation is UNORM too: the
  paint pipeline emits sRGB-encoded values (an authored `#2563eb` reaches the shader as
  `37/255, 99/255, 235/255`), so an `*_SRGB` presentation surface would encode them a second time and
  wash every color out. `Swapchain::format()` therefore reports a UNORM format on any surface that
  offers one; the sRGB siblings stay as the fallback for a surface that offers nothing else.

Rules:

- Descriptors are validated at creation, not at draw time.
- Zero-sized resources, invalid rows, invalid formats, and exceeding limits are errors.
- No implicit defaults for format/blend/sampler; the caller states them.
- Descriptor structs are plain data; no pointers except transient upload/creation data.
- B2 adds explicit `usage`/`stage` flags where the backend needs them; additions keep B1 fields
  source-compatible.

### 4.1 Blend modes, and dual-source coverage (RHI 1.5)

`PipelineDesc::blend` is one of:

| Mode | Blend |
|---|---|
| `None` | no blending: the fragment output replaces the destination |
| `Alpha` | `src.rgb * src.a + dst.rgb * (1 - src.a)`, alpha `src.a + dst.a * (1 - src.a)` |
| `DualSource` | per channel: `src0 * src1 + dst * (1 - src1)` |
| `DestinationIn` | `dst * src.a` on all four channels (RHI 1.19): the fragment colour is ignored; the mask step of an isolated layer, whose target holds premultiplied colour, so this is `mask-mode: alpha` compositing. Vulkan `ZERO`/`SRC_ALPHA`, the same factors on WebGPU and Metal, so no capability gates it |

`DualSource` (issue #471, [`display-aware-rendering.md`](../display-aware-rendering.md) section 3.4)
exists for LCD subpixel text, where the three coverage channels differ per pixel and one alpha
cannot express them. The fragment shader writes colour to output 0 and coverage to output 1 (GLSL:
`layout(location = 0, index = 1) out vec4`; SPIR-V: `Location 0` with `Index 1`), and the pipeline's
blend multiplies the two. `None` and `Alpha` are unchanged, so every existing caller keeps
byte-identical output.

It is a **capability**, not floor ([`backends.md`](backends.md) section 2.1): the fallback is
grayscale coverage, which is a correct picture, and the mode is optional on Vulkan.

- `Device::supports_dual_source_blend()` is the only authority. It is pure virtual so a backend cannot
  inherit a convenient answer; consumers must not infer it from `backend()`.
- When it reports false, `create_pipeline` with `DualSource` **throws** (`std::runtime_error` on
  Vulkan, `std::invalid_argument` on WebGPU). A backend must never map the mode to another blend: on
  Vulkan the SRC1 factors without the feature is undefined, and mapping it to "blend off" would write
  raw coverage instead of the fallback.
- Vulkan enables `VkPhysicalDeviceFeatures::dualSrcBlend` at device creation only when
  `vkGetPhysicalDeviceFeatures2` reports it, uses `VK_BLEND_FACTOR_SRC1_COLOR` /
  `VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR` for colour and alpha alike, and records the probe answer on
  the device (re-probed when a lost device is recovered).
- WebGPU reports false: it has one fragment output per color target and no SRC1 blend factor, so the
  mode is not expressible. The null backend reports true because it rasterizes nothing; it records
  the requested mode on the created pipeline instead, which is what a structural test asserts.

## 5. Command list

Order:

```text
create -> begin_render_pass -> bind_pipeline -> bind_vertex_buffer
  -> bind_texture(s) -> set_viewport/scissor -> push_constants -> draw
  -> end_render_pass -> copy target / copy region / blit -> submit
```

A command list that reads a render target as a texture (section 5.1) breaks that order once:

```text
... -> end_render_pass -> copy_target_region(...) -> bind_pipeline -> bind_sampled_target(slot, target)
  -> begin_render_pass ... -> push_constants -> draw -> end_render_pass ...
```

`bind_pipeline` is therefore legal outside a render pass, and `bind_sampled_target` must be recorded
outside one, after the pipeline.

Invariants:

- `begin_render_pass`/`end_render_pass` are balanced and never nested. A list may record more than
  one sequential pass (end, memory commands, begin again): that is the render-pass break/resume of
  the isolated-layer seam ([`offscreen-layers.md`](offscreen-layers.md)), and `begin_render_pass`
  starts the command buffer only once per submission. Beginning a pass while another is open is
  rejected by every backend (`std::runtime_error` on Vulkan, `std::logic_error` on null/WebGPU).
- A pipeline must be bound before a draw; vertex layout must match the bound buffer.
- `set_viewport`/`set_scissor` are explicit; there is no hidden default in the RHI.
- `push_constants` size must match `PipelineDesc::push_constant_size`.
- `copy_target_to_buffer` and `copy_target_region` happen outside the render pass.
- `copy_target_region(source, source_region, destination, destination_offset)` copies an integer
  pixel region 1:1 (no scale/filter) and is the RHI half of the isolated-layer seam
  ([`offscreen-layers.md`](offscreen-layers.md)): the batched renderer ends its pass, copies what
  it drew into an offscreen target, and continues. Both targets must be 1-sample, non-presentable,
  and format-compatible; source and destination must be different targets; `source` must have
  rendered content. `destination` ends readable in the state `end_render_pass()` leaves an offscreen
  target (`TRANSFER_SRC_OPTIMAL` on Vulkan), so a later copy/readback/resample can consume it.
  Invalid regions, formats, sample counts, presentable targets, a same-target copy, or an open pass
  are rejected loudly (`std::invalid_argument` / `std::runtime_error` or `std::logic_error` on
  WebGPU), never clipped or ignored. Swapchain images are not a copy source or destination in
  1.1; reading one needs `TRANSFER_SRC` usage at swapchain creation, which is a window decision.
- A list is submitted once, in one of two ways:
  - `submit_async()` queues the work and returns; this is the frame-path call.
    `is_complete()` polls without blocking and `wait(timeout_ms)` blocks for at most that long and
    returns whether the list finished. A timeout is a result, not an error.
  - `submit()` queues and waits for completion with no bound. It exists for offscreen renders,
    readback, and goldens; frame code on the UI thread must not call it ([ADR-021](../../../decisions/ADR-021-async-runtime.md)).
- Dropping a list that is still executing is allowed: the backend keeps its resources alive until
  the GPU is done with them.
- A command list is not recorded concurrently by multiple threads.
- The backend owns image-layout transitions, queue ownership, and API barriers; the RHI exposes
  no raw barrier calls in B1. A future explicit-barrier API needs a decision.

### 5.1 Sampled render target (RHI 1.3)

```cpp
virtual void bind_sampled_target(std::uint32_t slot, const RenderTarget& target) = 0;
```

- Binds a rendered, non-presentable target as a texture at descriptor `slot`, read with the sampler
  state it was created with (`RenderTargetDesc::filter`/`address`, default linear filtering and
  clamp-to-edge addressing). A 4x target is read through its resolved image, which is also what
  readback and a region copy observe.
- The state belongs to the target, so a bind allocates nothing on the frame path and a target is
  always read with the same state. Backends create the small objects (a Vulkan sampler and
  descriptor set) on the target's first bind and free them with the target.
- Record it outside a render pass, after `bind_pipeline`. Both halves of that order are backend
  constraints, not style: a dynamic-rendering instance cannot scope a barrier to the transfer stage
  the previous copy left the image in, and `vkCmdBindDescriptorSets` needs a pipeline layout.
  `bind_pipeline` before `begin_render_pass` is legal on every backend for the same reason.
- After the bind the target is in a backend-tracked shader-read state; a later render pass, copy,
  blit or readback transitions it back. The RHI exposes no layout and no barrier (`copy_target_to_buffer`
  and `copy_target_region` remain barrier-free by contract).
- Sampling the pass's own attachment is rejected - a target cannot be a sampled texture and a color
  attachment at once - as are a presentable target and a target without rendered content.
- The capability is effect-neutral: the glass composite and an isolated layer bind a target exactly
  like a texture, and no backend carries effect-specific code.

Backend notes: Vulkan keeps the readable image's layout on the target
(`TRANSFER_SRC_OPTIMAL` <-> `SHADER_READ_ONLY_OPTIMAL`; the 1-sample image, or a 4x target's resolve
image, carries `VK_IMAGE_USAGE_SAMPLED_BIT`) and transitions back to `TRANSFER_SRC_OPTIMAL` for
every later copy, blit, pass or readback. WebGPU's target texture already carries `TEXTURE_BINDING`
usage and the bind group pairs its view with the target's sampler; WebGPU tracks the transition.
Null records the bind (`NullSampledTargetBind`) and the ordered command log.

### 5.2 Texture mip chains (RHI 1.6)

`TextureDesc::levels` (default 1) asks for a mip chain. The chain is **built by the device at
creation from the uploaded base level** (`pixels` is required), so the texture is complete before
its first draw; there is no separate generate command, which keeps the upload on the one submission
the backends already make and leaves no window where a half-built chain can be sampled.

- Limits: `1 <= levels <= full_mip_levels(extent)`, only for `sample_count == 1`. Anything else
  throws `std::invalid_argument` (`valid_texture_levels`), on every backend.
- `Device::supports_texture_mipmaps(format)` is the only authority and is pure virtual. Vulkan
  reports it when the format allows blit source, blit destination and linear filtering with
  optimal tiling (the chain is a `vkCmdBlitImage` linear downsample); the null device reports true
  and records `Texture::levels()`; WebGPU reports true for every format it maps and builds the chain
  with one render pass per level (`shaders/mip_downsample.wgsl`, a fullscreen triangle sampling the
  level above with a linear sampler; issue #510). With false, `create_texture` with `levels > 1`
  throws and the caller uses one level.
- Sampling: the sampler's LOD range is `[0, levels - 1]`, so a one-level texture never samples
  elsewhere and a chain picks its level from the draw's minification (`texture()` derivatives).
  `mipmapMode` is linear when the texture's filter is linear, nearest otherwise.
- A mipmapped texture is **immutable**: `Texture::write` throws `std::invalid_argument`, because a
  write to level 0 would leave the lower levels stale.
- Memory accounting counts every level.

### 5.3 Texture arrays (RHI 1.7)

`CommandList::bind_texture_array(slot, textures)` binds an array of sampled images as one descriptor
set, so a shader can index them from a per-vertex value instead of one set per texture. It is the
seam that lets the batched 2D renderer keep one draw when consecutive quads sample different
textures (issue #480 part 2; the measured win is in
[`../renderer.md`](../renderer.md) "Texture arrays").

- `Device::texture_array_capacity()` is the only authority: the array length the shader's descriptor
  array was declared with, or 0 when the backend has no descriptor indexing. The default is 0, which
  is the one-set-per-texture path every existing caller keeps; WebGPU always answers 0.
- Vulkan enables it only when the physical device reports `runtimeDescriptorArray` plus the
  sampled-image indexing features, and then reports 32 - the length every Vulkan 1.2 implementation
  must support for a sampled-image array, so the shader's constant is portable.
- `PipelineDesc::texture_array` asks for a pipeline layout that declares the array. A backend without
  the capability throws rather than building a pipeline whose set can never be bound.
- `VertexFormat::UInt` carries the per-vertex index. The array is a **snapshot**: the caller
  re-binds it when the set of textures changes, at a batch boundary, never per quad. An empty array
  is a no-op on every backend; a longer one than the capacity throws `std::invalid_argument`.
- The set is read when the GPU executes the draw, not when it is recorded, so a caller must not
  rewrite one set between draws that are still in flight.

### 5.4 Texture region writes (RHI 1.9)

`Texture::write_region(region, data, row_pitch)` uploads one rectangle of an existing texture. It
exists because `Texture::write` is whole-image: a glyph that adds a few KB to a 2048x2048 atlas page
re-uploaded the 16 MiB page, and an upload scales with area, not with what changed (measured in
issue #681: a 2048 page write is 3.8-5.9 ms, a 512 page 0.2-0.4 ms).

- `region` is in whole texels from the top left. `data` points at the region's first texel and
  `row_pitch` is the byte distance between two source rows (0 = tightly packed), so a caller can
  pass a pointer into its own wider CPU copy (an atlas page) without repacking.
- One validation for every backend (`checked_region_pitch`, `texture_region.hpp`): a region outside
  the extent throws `std::out_of_range`; a pitch shorter than one region row or a null `data` for a
  non-empty region throws `std::invalid_argument`; an empty region is valid and writes nothing, so a
  caller that tracks a dirty rectangle does not special-case "nothing changed".
- Same restrictions as `write`: not valid for multisampled textures, and a mipmapped texture is
  immutable (section 5.2).
- Vulkan stages only the region's bytes (rows repacked into mapped staging memory when the source is
  pitched) and copies with an image offset, so staging and the transfer scale with the rectangle.
  WebGPU passes the origin and the pitch to `writeTexture`; Metal uses `replaceRegion`.
- The pure-virtual declaration is deliberate: a default that re-uploaded everything would hide the
  cost the call exists to remove.
- Consumer: `GlyphAtlas::take_dirty(page)` returns the union of the texels written since the last
  take (a new or reset page reports its whole extent), and `TextRenderer` uploads just that
  rectangle. Tests: `texture_write_region` (Vulkan pixel readback), the conformance check
  `texture_write_region` (every backend), `text_renderer_dirty_upload` (byte count equals the
  glyph's rectangle and the GPU copy equals the CPU page).

### 5.5 Read-only storage buffers (RHI 1.12)

`CommandList::bind_storage_buffer(slot, buffer)` binds a `BufferUsage::Storage` buffer as a
read-only shader resource, and `PipelineDesc::storage_buffer` declares the binding in the pipeline
layout. It exists because a push constant is a fixed-size block: the clip set `clip-path` needs is a
variable-length record list, and no push-constant budget holds one
([ADR-068](../../../decisions/ADR-068-clip-state-in-a-storage-buffer.md)).

- `Device::supports_storage_buffers()` is the only authority. It is pure virtual, like
  `supports_dual_source_blend()`, so every backend answers. A backend answering false keeps the
  push-constant path with its three rounded rectangles, and `create_pipeline` with
  `PipelineDesc::storage_buffer` throws instead of building a pipeline whose binding can never be
  satisfied. A Tier-2 backend may answer false indefinitely; it is a capability, not a deadline.
- A zero-size buffer is refused at creation (`std::invalid_argument`), not at bind: Vulkan cannot
  create one at all (`vkCreateBuffer` reports `VK_ERROR_INITIALIZATION_FAILED`), so the RHI states
  the rule once instead of letting each backend fail differently.
- `bind_storage_buffer` throws `std::logic_error` without a bound pipeline, and
  `std::invalid_argument` for a buffer not created with `BufferUsage::Storage`.
- The storage buffer is a **separate descriptor set** from the sampled image, so a shader can sample
  a texture and read the record list in the same draw. `slot` names that set; Vulkan and null use
  set 1 (set 0 is the sampled image), and a slot the layout does not declare throws
  `std::invalid_argument`.
- The buffer's contents are read at draw time, not at bind time, so a caller may rewrite it between
  draws. It must not be rewritten while a submission that reads it is in flight; the
  per-frame-buffer pattern (one buffer per frame in flight) is what keeps that true.
- Vulkan uses a long-lived `VK_DESCRIPTOR_TYPE_STORAGE_BUFFER` set rewritten in place
  (update-after-bind), the same pattern as the texture array (section 5.3), so a frame records no
  descriptor allocation. The capability is gated on
  `VkPhysicalDeviceLimits::maxPerStageDescriptorStorageBuffers`, which is the only honest limit: a
  fragment-stage storage buffer is core Vulkan 1.0, so there is no feature bit to enable.
- WebGPU and Metal report false until ADR-068 slice 2 implements the binding; both have an
  expressible form (`read-only-storage`, `[[buffer(n)]]`), so the answer is a schedule, not a limit.
- Tests: the conformance check `storage_buffer_bind` (a fragment shader reads the buffer and the
  target is read back for its colour, on every backend that reports the capability) and the null
  structural test in `rhi_null_tests` (the recorded slot and size, plus the three refusals).

### 5.6 Per-category GPU memory (RHI 1.14)

`DeviceMemoryStats::categories` splits the live GPU bytes by *purpose* instead of by resource kind,
and adds a high-water mark per category. memory-model-v1 section 4 asks for explicit per-category
budgets; `buffer_bytes` and `image_bytes` say how much the device holds, not what it holds it for, so
a texture regression and a vertex-buffer regression were indistinguishable (issue #703).

- `MemoryCategory` is a separate enum from `BufferUsage`, not a reuse of it: a texture has no
  `BufferUsage`, and the categories span both resource kinds. `TextureDesc::category` is how a
  texture names its purpose; a buffer's `BufferUsage` already is its purpose, so the Vulkan backend
  maps it rather than asking for a second field.
- The default is `Other`, and that is deliberate. A caller that does not care is *counted and
  visible* rather than silently attributed to a category it may not belong to. The glyph atlas and
  the path cache set their own categories because each owns a bounded pool with its own budget row,
  and the point of the split is to compare that row against the device's own accounting instead of a
  second, drifting tally.
- `peak_bytes` is updated on the way up only, so it survives the free that follows a transient
  allocation. That is the whole reason it exists: a staging buffer that lives for one upload is
  invisible to a live-bytes sample taken after the upload.
- The two views are not additive with each other. `buffer_bytes + image_bytes` and the sum of
  `categories` both describe the live set, so a backend filling both must keep them consistent;
  `rhi_memory` asserts exactly that, and that releasing everything returns every category to zero.
- A backend that cannot attribute a resource reports zeros rather than a guess. The null backend
  does, and `rhi_memory` asserts it: a budget row measured there would otherwise read as evidence for
  a number nobody measured.
- Enforcement: `tests/benchmarks/memory/gpu_memory_probe.cpp` measures a fixed resource set and the
  `gpu_vma_category_bytes` / `gpu_category_peak_after_free_bytes` rows of
  `tests/golden/perf/memory_budget.json` ratchet it. The numbers are deterministic because they are
  *our* accounting (extent x texel size, buffer size), not the driver's block sizes, so lavapipe and
  a real GPU agree and the row can be enforced. The allocator's own `block_bytes` is deliberately not
  the budgeted row: it is driver-specific and would make the gate flaky.

### 5.7 Buffer to texture copy (RHI 1.17)

`copy_buffer_to_texture(source, destination, BufferTextureCopy{region, buffer_offset, buffer_row_pitch})`
copies bytes into a texture rectangle inside the command list, so a compute pass's output reaches a
texture a draw samples with no CPU round trip and no extra submission
([ADR-072](../../../decisions/ADR-072-rhi-copy-buffer-to-texture.md)).

- **Bytes are verbatim.** The buffer holds the texture's own layout (`R8G8B8A8Unorm`: R, G, B, A;
  `R8Unorm`: one byte); a shader writes the channel order and transfer function the texture expects.
- **Source:** a `Staging` or `Storage` buffer (a `gpu_read_write` one is the compute output).
  **Destination:** a one-sample, one-level texture.
- **Layout:** rows are `buffer_row_pitch` bytes apart starting at `buffer_offset`. The offset is a
  multiple of 4; the pitch a multiple of `kTextureCopyRowAlignment` (256, WebGPU's `bytesPerRow`, the
  strictest backend) and at least one row. 0 means `aligned_texture_copy_pitch(width, format)`.
- **Order:** after the compute work recorded before it (the backend inserts the barrier), before any
  draw recorded after it. It is not recorded inside a render pass (`std::logic_error`).
- **Errors:** the region outside the texture is `std::out_of_range`; a bad pitch or offset, a buffer
  too short for the rows, a wrong usage, or a multisampled/mipmapped destination is
  `std::invalid_argument`; a stale device epoch or a backend without the capability is
  `std::runtime_error`. An empty region copies nothing. `checked_buffer_texture_copy`
  (`texture_region.hpp`) is the one shared check, so every backend refuses the same inputs.
- **Tests:** `buffer_texture_copy_null` (validation matrix, record, order) and
  `buffer_texture_copy` (staging, storage and compute-fed pixels read back on a real device).

## 6. Lifetime and threading
- The device owns GPU objects; users own handles and destroy them before the device.
- Resources referenced by in-flight frames retire after their completion value; never destroy
  immediately after submit.
- B1 baseline: one thread. `submit_async` and bounded `wait` are in place; B2 may add a render
  thread, where the UI thread records immutable command data and never blocks indefinitely.
- `Device` state (recycled command resources, pending uploads, deferred destruction) is not
  thread-safe: use one device from one thread at a time until the render-thread work lands.
- Mixing objects from different backends is a programming error. Debug builds assert; release
  builds do not check (the frame path uses `static_cast`, not RTTI).
- Cross-thread ownership transfer is explicit; no hidden locking in the steady-state path.
- Hot reload/resize creates a new generation; old generation retires after frames in flight.
- Shutdown: stop new work, bounded wait, retire resources, then destroy the device.

## 7. Errors and failure

- Creation returns an owning handle or an explicit error; no nullable handle is treated as valid.
- B2 target: `std::expected<Handle, RhiError>` with stable categories:
  `invalid-argument`, `unsupported`, `out-of-memory`, `device-lost`, `validation`, `backend`.
- `Buffer::write`/`read` never fail silently: an out-of-range request throws `std::out_of_range`
  (the bounds check cannot overflow) and a failed mapping throws `std::runtime_error`. Both the
  Null and Vulkan backends behave the same.
- A `create_*` call that fails part-way releases everything it already created.
- Device loss is sticky: all new submissions fail until recreated; UI state is preserved.
- Out-of-memory must evict caches/fallbacks before failing the app.
- Validation errors are fatal in debug/tests; release behavior is backend-defined but logged.
- Errors never mutate UI/scene state directly; the runtime decides fallback/recovery.

## 8. Null backend

The null backend implements the same object/command API with no GPU. It records:

- render-pass begin/end
- pipeline/vertex/texture binds, including `bind_sampled_target` with the sampled target's slot,
  extent, format, sample count and sampler state
- the state a created pipeline asked for (`NullPipeline::sample_count`/`blend_mode`), so a pipeline
  request - including `BlendMode::DualSource`, which has no pixels to inspect - is assertable
- viewports/scissors
- push-constant size
- draw calls/vertices
- copies/submits, including the region and destination offset of `copy_target_region`
- `copy_buffer_to_texture` (RHI 1.17): the source usage, destination extent, region, offset and
  effective pitch; the bytes really land in the null texture
- every recorded operation in command order (`NullCommandStats::command_order`), so a structural
  test can assert a sequence - the composite seam's attachment -> copy -> sample -> composite - and
  not only counts

Null is the default unit/structural test path. It must reject invalid command order the same way
a validating backend would, so tests catch contract errors without a GPU.

## 9. Determinism and tests

- No wall clock, random ids, or driver-dependent defaults in the RHI contract.
- The same command script must produce identical null stats across platforms.
- Contract tests run against null, software/lavapipe, and real Vulkan where available.
- Resource lifetime tests use fake frame completion values, not sleeps.
- See [`testing.md`](testing.md).

## 10. Deferred/open

- Explicit resource-state/barrier API vs backend-owned transitions.
- Same-target (in-image) region copies, and a swapchain target as a copy source or destination for
  the layer seam (needs `TRANSFER_SRC`/`TRANSFER_DST` usage at swapchain creation).
- Bindless/descriptor indexing.
- Compute and transfer queue families.
- Timestamp queries and GPU timing.
- Indirect draws and multi-draw.
- Device-loss recreation policy per platform.
