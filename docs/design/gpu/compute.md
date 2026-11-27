# RHI compute

- Status: Vulkan, WebGPU (RHI 1.22) and the opt-in null device implemented; Metal open.
- Issues: #683 (the spike and 1.16), #684 (this completion). Decision: [ADR-071](../../../decisions/ADR-071-gpu-compute-tile-rasterizer.md)
  (Accepted with conditions), [ADR-072](../../../decisions/ADR-072-rhi-copy-buffer-to-texture.md) (the handoff to a texture).
- Terms: a **workgroup** is the group of shader invocations that share memory and barriers; an
  **indirect dispatch** reads its workgroup counts from a buffer when the GPU executes it.

Compute is recorded outside a render pass, in the same command list and submission as the passes
around it. Nothing here waits on the UI thread: a result comes back through `copy_buffer` into a
`Readback` buffer after the submission completes, or stays on the GPU for a draw to sample
(`copy_buffer_to_texture`).

## 1. Surface

| Piece | Version | Notes |
|---|---|---|
| `Device::supports_compute()` | 1.16 | The only authority; callers ask it first. |
| `ComputePipelineDesc`, `create_compute_pipeline` | 1.16 | SPIR-V only, 1 to 8 read/write storage buffers, push constants. |
| `BufferDesc::gpu_read_write` | 1.16 | Device-memory storage buffer a shader writes; not host-readable. |
| `bind_compute_pipeline`, `bind_compute_buffer`, `compute_push_constants` | 1.16 | One descriptor set per pipeline object. |
| `dispatch(x, y, z)`, `compute_barrier`, `copy_buffer` | 1.16 | Counts 1 to 65535 per axis. |
| `ComputeCapabilities`, `Device::compute_capabilities()` | 1.18 | Workgroup size and count limits, shared memory, optional subgroup size, indirect dispatch, buffer atomics. |
| `CommandList::dispatch_indirect(buffer, offset)` | 1.18 | Three `uint32` counts at `offset`; buffer is `Storage`, offset a multiple of 4. |

A backend that cannot do compute keeps the defaults: `supports_compute()` false, zeroed capabilities,
every compute call throws `std::runtime_error`. Capabilities are never inferred from the backend name.

## 2. Contracts

- **Barriers.** `compute_barrier` makes every storage write recorded so far visible to later dispatches
  and to indirect argument reads, so the pass that writes the counts of a `dispatch_indirect` is ordered
  before it by one barrier. A dispatch that reads another's output needs one between them.
- **Indirect arguments.** Every `Storage` buffer carries the indirect-argument usage, so a CPU-written
  or compute-written buffer can size a dispatch without a second buffer type. A misaligned offset, one
  with fewer than 12 bytes behind it, or a non-`Storage` buffer throws `std::invalid_argument`.
- **Atomics.** 32-bit integer atomics on a storage buffer are core on every device the Vulkan backend
  accepts; `buffer_atomics` reports it for the backends that will not.
- **Storage images.** Not part of this surface. The path rasterizer writes a storage buffer and hands it
  to a texture with `copy_buffer_to_texture` (ADR-072), which needs no image-write capability. A
  storage-image write is added when a consumer needs one, with its own capability bit.
- **Limits.** `max_workgroup_count` never exceeds the 65535 that `dispatch` validates, so a shader
  sized from it is always accepted.

## 3. Backends

| Backend | State |
|---|---|
| Vulkan | Implemented: `vulkan_compute.cpp`; limits probed once at device creation. |
| Null | `create_null_device` answers no compute (so `GpuPathRasterizer::supported` keeps its CPU route). `create_null_compute_device` answers true with a fixed capability set, validates like Vulkan and records every call in `NullCommandStats` (`command_order`, `dispatches`, counters); `copy_buffer` moves the bytes. |
| WebGPU | Implemented (RHI 1.22): `webgpu_compute.cpp`; pipelines from the WGSL registry, see below. Verified against a real device through Dawn (section 4). |
| Metal | Open: MSL entry. Defaults until then. |

### 3.1 WebGPU

The pipeline is named, not compiled from SPIR-V: `ComputePipelineDesc::shader_id` (RHI 1.22) is looked up in
the WGSL `ShaderRegistry` and the program's `compute_entry` is the entry point, the way `PipelineDesc::shader_id`
names a render program. The contract a compute WGSL program follows:

- Storage buffers are group 0, bindings 0 to `storage_buffer_count - 1`, **every one `var<storage,
  read_write>`**. WebGPU rejects a pipeline whose layout and declared access disagree, and the RHI binds one
  kind of buffer, so a buffer a kernel only reads is still declared `read_write`. A buffer used with
  `atomicAdd` is declared `array<atomic<u32>>`, which WGSL requires.
- Push constants do not exist in WebGPU. The block is a uniform at group 0, **binding 8**
  (`kBindingComputePush`), written per `compute_push_constants` call into a per-list arena and bound with a
  dynamic offset, so changing it between dispatches builds no new bind group. A pipeline that declares push
  constants and gets none reads zeros.
- `compute_barrier` ends the compute pass, so the order it documents never depends on WebGPU's own
  in-pass ordering. `copy_buffer`, a render pass and the submit end it too.
- A pipeline's bindings belong to the pipeline object, as on Vulkan: binding a different pipeline starts with
  no buffers, and every slot must be bound before a dispatch (a missing one throws `std::logic_error`). The
  bind group layout holds all `storage_buffer_count` slots even when an entry point declares fewer, so a pass
  that does not use a buffer still binds one (the path rasterizer binds all seven for each of its four passes;
  Vulkan ignores the extras).
- `copy_buffer_to_texture` (ADR-072 slice 3) is implemented: the RHI's 256-byte row pitch is WebGPU's
  `bytesPerRow` rule, so the region maps to `wgpuCommandEncoderCopyBufferToTexture` directly, ending the compute
  pass first. `supports_buffer_texture_copy()` is true, which is what lets `GpuPathRasterizer` run.
- `copy_buffer` needs a size that is a multiple of 4 (the API copies words); a Storage buffer carries the
  copy-source, copy-destination and indirect usages, so it can be filled by the queue, copied out and used as
  `dispatch_indirect` arguments. A buffer must not be bound as storage **and** be the indirect buffer of the
  same dispatch (WebGPU's usage-scope rule); the conformance kernels keep the two apart.
- Limits come from the device's own limits (`wgpuDeviceGetLimits`), not the adapter's best: a default device
  reports 256 invocations and 16 KiB of shared memory. `max_workgroup_count` is clamped to 65535, the number
  `dispatch` validates everywhere. `subgroup_size` is 0 (not core WebGPU).
- Readback is asynchronous only (`WebGpuBuffer::read` throws): copy into a `Readback` buffer, wait for the
  submission, then map it.

## 4. Conformance

`tests/gpu/rhi/compute_conformance_tests.cpp` (ctest `compute_conformance`) with the kernels in
`compute_conformance.comp`, whose output is exactly computable on the CPU:

- one invocation writes the arguments of an indirect dispatch, which then runs the histogram;
- a 16-bin histogram with `atomicAdd` over 1000 values (63 in each of the first 8 bins, 62 in the rest);
- an exclusive prefix sum in shared memory over 200 values.

The null half runs everywhere and asserts the recorded stream and validation. The Vulkan half runs on
any Vulkan device, lavapipe included, and skips without one. `compute_dispatch` keeps the original
probe (dispatch, barrier, readback, misuse). Every backend that adds compute runs the same kernels.

**WebGPU.** `webgpu_compute_conformance` (`backend/webgpu/smoke/`, kernels in `shaders/compute_conformance.wgsl`)
runs the same three kernels with the same exact expectations. It needs a real WebGPU device, which the host
injects as `Module.preinitializedWebGPUDevice`, so the Wasm lane builds it (the `wasm-webgpu` build preset) but
cannot run it under plain Node. `scripts/webgpu_compute_conformance.mjs` runs it on a GPU under Node with
Dawn's `webgpu` npm bindings (installed outside the repository; the script's header has the commands). It is
built with JSPI, because ASYNCIFY cannot be mixed with the wasm exceptions the lane compiles with. Measured
on 2026-11-27 on an NVIDIA RTX 4060 through Dawn on D3D12 (Node 25.2.1): the indirect arguments, all 16 histogram
bins and the 200 prefix sums matched, with no validation errors. A browser run is the same program in a page.

**Path rasterizer.** `webgpu_path_raster_conformance` (same host script, `TESSERA_CONFORMANCE_JS` pointing at it;
`--goldens <dir>` for the PNGs, `--measure [--dense]` for the 4K frame) runs the corpus of
`tests/gpu/batched2d/path_raster_corpus.hpp` through `webgpu::path_raster_pipeline_descs()` (four programs of
`shaders/path_raster.wgsl`, seven `read_write` storage buffers, the push block at binding 8) and compares the
readback with the lavapipe goldens under the ThorVG limits of ADR-071. The results (RTX 4060 through Dawn on
D3D12: every scene within 1 premultiplied level) and the 4K timings are in ADR-071, section "WebGPU numbers
(Dawn)". `TESSERA_DAWN_MAX_LIMITS=1` asks the device for the adapter's own storage-binding and buffer limits (a 4K
raster's buffers exceed the 128 MiB default binding size).
