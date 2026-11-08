# Renderer and RHI design (Big Step B1)

Status: B1 implemented. B2 adds Yoga layout, text shaping/glyph atlas, dirty rendering, and
pixel goldens. B3 adds the window/swapchain path with frame pacing and dynamic refresh.

See also: [`game-loop.md`](game-loop.md), [`dirty-rendering.md`](dirty-rendering.md),
[`input.md`](input.md), [`window-resize.md`](window-resize.md), [`async.md`](async.md),
[`diagnostics.md`](diagnostics.md), [`resources.md`](resources.md),
[`threading-and-memory.md`](threading-and-memory.md), [`layout.md`](layout.md),
[`text.md`](text.md), [`antialiasing.md`](antialiasing.md),
[`stylesheet-v1.md`](../specs/stylesheet-v1.md), [`runtime-config-v1.md`](../specs/runtime-config-v1.md),
and [`custom-elements-v1.md`](../specs/custom-elements-v1.md).

## Layers

| Layer | Location |
|---|---|
| Scene graph / events | `include/tessera/ui/scene/`, `src/ui/scene/` |
| Batched 2D renderer | `include/tessera/rendering/gpu/`, `src/rendering/gpu/` |
| Custom elements | `include/tessera/element/`, `src/element/` (planned) |
| Thin RHI | `include/tessera/rendering/rhi/` |
| Null backend | `src/rendering/null/` |
| Vulkan backend | `src/rendering/backend/vulkan/` |
| WebGPU backend | `src/rendering/backend/webgpu/` (B3) |
| Demo | `tools/poc/batched2d/` |

The UI layer never includes Vulkan, volk, VMA, or WebGPU headers. The only GPU seam is
`rhi::Device` / `rhi::CommandList`. Custom elements use that same RHI seam; they do not get raw
backend handles.

## RHI surface

- Opaque resources: `Buffer`, `Texture`, `Pipeline`, `RenderTarget`, `CommandList`, `Device`.
- Descriptors: `BufferDesc`, `TextureDesc`, `PipelineDesc`, `RenderTargetDesc`.
- `CommandList` is explicit: begin/end render pass, bind pipeline/vertex buffer/texture,
  viewport/scissor, push constants, draw, copy target to buffer, submit.
- B1 command lists are synchronous one-shot submissions; the backend owns the fence wait.
- No hidden global state and no implicit barriers. The batched renderer owns batch and clip
  policy; the backend owns image-layout transitions for its own targets/textures.
- Lifetime rule: all resources must be destroyed before their `Device`. The demo declares the
  device first so reverse destruction is correct.

## Batched 2D renderer

`gpu::Vertex2D` is 44 bytes:

| Field | Format |
|---|---|
| position | `R32G32_SFLOAT` |
| uv | `R32G32_SFLOAT` |
| color | `R8G8B8A8_UNORM` |
| local | `R32G32_SFLOAT` |
| half_size | `R32G32_SFLOAT` |
| radius | `R32_SFLOAT` |
| mode | `R32_SFLOAT` |

Modes: `0` solid, `1` SDF rounded rectangle, `2` textured quad.

Rules:

- **Batching key:** `(scissor, texture)`. Consecutive quads with the same key share one draw.
  Painter's order is preserved; there is no depth buffer.
- **Clip stack:** axis-aligned scissor rectangles, intersected on push. Rounded/path clips are B2+.
- **Transform stack:** 2D affine matrices. Positions are transformed on the CPU, so transforms do
  not break batching. Local coordinates stay in the rectangle's space so the SDF rotates/scales
  with the quad.
- **SDF rounded rects:** fragment shader evaluates a rounded-box signed distance. AA radius is
  1 px normally; `--golden` sets it to 0 for hard, pixel-stable edges.
- **Textures:** one combined image sampler per texture; solid/SDF quads bind a 1x1 white texture.
  A glyph atlas will reuse the same path.
- **No `discard`:** transparent SDF fragments write alpha 0 and vanish through alpha blending.
  This avoids a dependency on `shaderDemoteToHelperInvocation`.

## Vulkan backend

- Vulkan 1.3 baseline with dynamic rendering; no legacy render passes or framebuffers.
- **volk** loads instance/device entry points dynamically; **VMA** owns buffer/image memory.
- One graphics queue and one frame in flight in B1.
- Buffers are host-visible in V1 (vertex, staging, readback). The readback buffer is
  `TRANSFER_DST` with host-random access.
- Textures are `SAMPLED | TRANSFER_DST`, uploaded through a staging buffer, then transitioned to
  `SHADER_READ_ONLY_OPTIMAL`. Each texture owns its sampler and descriptor set.
- Render targets are `COLOR_ATTACHMENT | TRANSFER_SRC`; `R8G8B8A8_UNORM` in B1.
- Frame sequence: transition target to `COLOR_ATTACHMENT_OPTIMAL`, `vkCmdBeginRendering`, draw
  batches, `vkCmdEndRendering`, transition to `TRANSFER_SRC_OPTIMAL`, copy to readback buffer,
  submit, fence-wait, map/read.
- Validation layers are opt-in (`--validation`, debug builds). The demo runs clean.
- Required Vulkan 1.3 features: dynamic rendering. No discard/demote requirement.

## Custom element rendering

- `CustomElement` is a leaf node implemented by a native C++ class; it can record custom
  pipelines, vertex/index buffers, compute, and 3D draws through the RHI.
- **Offscreen** (default): render into a framework-managed color/depth target sized to the node,
  then composite as a texture. Preserves 2D ordering/clipping and supports transparency.
- **Inline** (advanced): record into the current frame target at the node viewport with a depth
  attachment. Opaque/full-bleed only; no 2D children; depth clear/order constraints apply.
- `onDemand` renders on changes; `continuous` animates while visible and pauses when
  hidden/occluded/backgrounded.
- Targets/pipelines are cached; no per-frame pipeline creation; old targets retire after frames
  in flight. No raw backend/OS handles, no runtime shader compilation.
- See [`custom-elements-v1.md`](../specs/custom-elements-v1.md).

## Null backend

Records render-pass, bind, scissor, draw, and vertex counts. It has no GPU and no image output; it
exists so batching, clipping, transforms, and command sequencing can be tested on any machine.

## Runtime integration (B2/B3)

- **B1:** offscreen target, one frame in flight, full redraw, synchronous submit/wait.
- **B2:** fixed-step loop, dirty style/layout/paint/render, retained draw lists/layers, bounded
  caches, damage rectangles on the offscreen path.
- **B3:** window + swapchain, acquire/present, `maxFps`, vsync modes, dynamic refresh/VRR,
  incremental present/damage where the platform supports it; resize/DPI/lifecycle and
  old-swapchain retirement follow [`window-resize.md`](window-resize.md).
- Async uploads/resource loads arrive through the UI-thread completion queue
  ([`async.md`](async.md)); diagnostics/tracing wrap the frame and GPU passes
  ([`diagnostics.md`](diagnostics.md)).
- Configuration lives in `tessera.json.runtime`; see
  [`runtime-config-v1.md`](../specs/runtime-config-v1.md).
- Deterministic/golden mode always full-redraws into a fixed offscreen target and ignores
  pacing/present settings.

## Demo and reference image

```bash
batched2d_poc --backend vulkan --width 512 --height 512 --out batched2d.png
batched2d_poc --backend null
```

Reference: [`docs/images/batched2d.png`](../images/batched2d.png).

`--golden` disables AA. A pixel golden pipeline on lavapipe/software (ADR-009) is B2/B3 work.

## B2 / known limits

- B1 is offscreen only; no window or swapchain.
- B1 always full-redraws; dirty caches/damage are B2/B3.
- One frame in flight; no timeline semaphores or multi-frame pipelining yet.
- No FPS cap, vsync/VRR, or dynamic refresh yet; these are B3 window-path features.
- No Yoga layout; scene nodes use explicit bounds.
- No text shaping or glyph atlas.
- No vector path rasterizer (ADR-004).
- No sRGB/color-management policy yet (ADR-001 follow-up).
- No bindless/descriptor indexing; one descriptor set per texture.
- Pixel goldens from lavapipe/software are not wired yet.
