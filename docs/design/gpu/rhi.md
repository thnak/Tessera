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
  (`1.0` / `0x00010000` for the first published version). The current contract is **1.4**
  (`0x00010004`): 1.0 is the B1 surface; 1.1 adds `CommandList::copy_target_region` for the
  isolated-layer seam (issue #242, [`offscreen-layers.md`](offscreen-layers.md)); 1.2 adds the
  swapchain composite-alpha request and the resolved-mode query (glass-v1.md section 6, issue #243);
  1.3 adds the sampled view of an offscreen render target (`CommandList::bind_sampled_target` plus
  the target's sampler state, issue #254, section 5.1 below); 1.4 adds
  `DeviceDesc::surface`/`SurfacePlatform`, the presentation surface family a device is created for
  (issue #155, Wayland window + `VK_KHR_wayland_surface`). All later versions are additive
  compatible changes.
- `RhiCapabilities { Backend backend; std::uint32_t rhi_version; GpuTimestampCapability timestamps; }`
  is reported by `Device::capabilities()` (default: current backend, `kRhiVersion`,
  `timestamp_capability()`). Backends declare the implemented major version with a compile-time
  `static_assert` against `kRhiVersionMajor` and report it via capabilities for runtime/conformance
  logging. The type is not named `DeviceCapabilities` because `<wingdi.h>` defines that word as a
  macro; the public RHI headers stay includable beside Windows headers.
- A PR that changes `include/tessera/rendering/rhi/**` must update `version.hpp` and this contract
  doc to state the version impact; the docs gate fails RHI header changes without those matching
  updates.

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

Rules:

- Descriptors are validated at creation, not at draw time.
- Zero-sized resources, invalid rows, invalid formats, and exceeding limits are errors.
- No implicit defaults for format/blend/sampler; the caller states them.
- Descriptor structs are plain data; no pointers except transient upload/creation data.
- B2 adds explicit `usage`/`stage` flags where the backend needs them; additions keep B1 fields
  source-compatible.

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
- viewports/scissors
- push-constant size
- draw calls/vertices
- copies/submits, including the region and destination offset of `copy_target_region`
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
