# GPU rendering docs

Status: started. B1 (offscreen Vulkan + null backend) is implemented; B2 dirty/golden and B3
swapchain/present work is partially implemented (see [`../../README.md#8-status`](../../README.md#8-status));
the contracts below remain the target. This folder is the detailed GPU/RHI design set;
[`../renderer.md`](../renderer.md) remains the B1 summary and acceptance record.

## 1. Scope

- The RHI seam (`include/tessera/rendering/rhi/`).
- Frame execution, frames in flight, synchronization, uploads/readbacks.
- GPU resources, memory, pipelines, descriptors, shader manifests.
- Vulkan, WebGPU, null, and software/lavapipe paths.
- GPU tests/goldens and failure handling.

Not here: scene graph, stylesheet/layout, text shaping, vector paths, window/swapchain policy,
or asset decoding. Those have their own docs.

## 2. Document map

| Document | Owns |
|---|---|
| [`rhi.md`](rhi.md) | Device/resource/command contract, lifetime, errors |
| [`frames.md`](frames.md) | Frame lifecycle, in-flight frames, barriers, uploads, resize |
| [`resources.md`](resources.md) | Buffers/textures/targets/samplers/memory |
| [`pipelines.md`](pipelines.md) | Pipeline cache, descriptors, vertex layout, shaders |
| [`backends.md`](backends.md) | Vulkan/WebGPU/null/software parity and capabilities |
| [`webgpu-backend.md`](webgpu-backend.md) | WebGPU backend first slice, status, remaining work |
| [`metal-backend.md`](metal-backend.md) | Metal mapping: sync, resource lifetime, `CAMetalLayer`, errors |
| [`testing.md`](testing.md) | Test pyramid, tree, goldens, CI |

Related: [`../threading-and-memory.md`](../threading-and-memory.md),
[`../resources.md`](../resources.md), [`../dirty-rendering.md`](../dirty-rendering.md),
[`../game-loop.md`](../game-loop.md), [`../window-resize.md`](../window-resize.md),
[`../../specs/shader-manifest-v1.md`](../../specs/shader-manifest-v1.md),
[`../../specs/memory-model-v1.md`](../../specs/memory-model-v1.md),
[`../../specs/buffer-config-v1.md`](../../specs/buffer-config-v1.md),
[`../../specs/golden-tests-v1.md`](../../specs/golden-tests-v1.md), and
[`../../specs/custom-elements-v1.md`](../../specs/custom-elements-v1.md).

## 3. Layers and ownership

```text
UI / batched 2D / custom elements       (no backend headers)
        |
        v
RHI: Device, CommandList, resources     (only GPU seam)
        |
        v
Backend: Vulkan | WebGPU | Null         (owns API objects/barriers/memory)
```

| Layer | Owns | Must not do |
|---|---|---|
| UI/renderer | draw lists, batching, clip/transform, pass order | include Vulkan/WebGPU headers |
| RHI | opaque resources, command recording, validation | expose backend handles |
| Backend | API objects, image layouts, queues, fences, VMA | mutate UI state |
| Resource cache | asset ids, residency, eviction | create per-frame pipelines |
| HAL/window | surface/swapchain/display | record UI draws |

## 4. Invariants

1. The RHI is the only GPU seam; UI and custom elements never receive raw backend handles.
2. No hidden global state, implicit bindings, or implicit barriers in the RHI contract.
3. Resources are destroyed before their `Device`; in-flight frames defer destruction.
4. No runtime shader compilation; shaders/pipelines come from build outputs/manifests.
5. No per-frame pipeline/resource allocation in the steady state.
6. A frame's draw lists/batches are immutable once submitted.
7. Static screens do not submit GPU work; the loop never busy-waits.
8. Null/software paths must exercise the same command/validation path as real backends.
9. A device/backend failure is explicit and recoverable; it never corrupts UI state.
10. Deterministic mode uses fixed inputs, fixed offscreen targets, and no real-GPU baselines.

## 5. Phases

| Phase | GPU scope |
|---|---|
| B1 (done) | Thin RHI, null backend, Vulkan offscreen, batched 2D, readback, smoke test |
| B2 | Frames in flight, timeline/fence sync, dirty render/damage, goldens, device loss handling |
| B3 | Window/swapchain acquire/present, resize/DPI, old-swapchain retirement |
| C/D | Wasm/WebGPU parity, designer preview, shared shader/golden corpus |

## 6. File and test layout

- Design docs in this folder stay under 600 lines; split by responsibility instead of growing a
  single file. An index links the parts.
- GPU tests live under [`tests/gpu/`](../../../tests/gpu/README.md); goldens under
  `tests/golden/gpu/`, integration under `tests/integration/gpu/`, fixtures under
  `tests/fixtures/gpu/`, benchmarks under `tests/benchmarks/gpu/`.
- B1 GPU tests are already under `tests/gpu/rhi/` and `tests/gpu/batched2d/`; new GPU tests do
  not go in `tests/unit/`.

## 7. Open decisions

- B2 persistent pipeline cache and its versioning.
- Bindless/descriptor indexing (deferred; one set per texture in B1).
- Compute shader scope in V1.
- sRGB/color-management policy (no decision yet).
- WebGPU reflection and shader parity tooling.
- Device-loss UX/fallback policy per platform.
