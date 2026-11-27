# RHI compute

- Status: Vulkan and the opt-in null device implemented (RHI 1.16 to 1.18); WebGPU and Metal open.
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
| WebGPU | Open: WGSL entry and a compute bind group. Defaults until then. |
| Metal | Open: MSL entry. Defaults until then. |

## 4. Conformance

`tests/gpu/rhi/compute_conformance_tests.cpp` (ctest `compute_conformance`) with the kernels in
`compute_conformance.comp`, whose output is exactly computable on the CPU:

- one invocation writes the arguments of an indirect dispatch, which then runs the histogram;
- a 16-bin histogram with `atomicAdd` over 1000 values (63 in each of the first 8 bins, 62 in the rest);
- an exclusive prefix sum in shared memory over 200 values.

The null half runs everywhere and asserts the recorded stream and validation. The Vulkan half runs on
any Vulkan device, lavapipe included, and skips without one. `compute_dispatch` keeps the original
probe (dispatch, barrier, readback, misuse). Every backend that adds compute runs the same file.
