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
DeviceDesc { Backend backend; bool validation; int device_index; }
Device::backend();
Device::wait_idle();
```

RHI versioning and capabilities (ADR-049):

- `include/tessera/rendering/rhi/version.hpp` defines the published RHI contract version as
  `kRhiVersionMajor`, `kRhiVersionMinor`, and `kRhiVersion = (major << 16) | minor`
  (`1.0` / `0x00010000` for the first published version). The current contract is **1.1**
  (`0x00010001`): 1.0 is the B1 surface, 1.1 adds `CommandList::copy_target_region` for the
  isolated-layer seam (issue #242, [`offscreen-layers.md`](offscreen-layers.md)).
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
| `RenderTargetDesc` | `extent`, `format`, `clear_color`, `load_op`, `store_op` |
| `PipelineDesc` | vertex/fragment SPIR-V, vertex layout, color format, blend, push-constant size |

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
- pipeline/vertex/texture binds
- viewports/scissors
- push-constant size
- draw calls/vertices
- copies/submits, including the region and destination offset of `copy_target_region`

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
