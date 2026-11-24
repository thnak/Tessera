# ADR-071: A GPU compute tile rasterizer for vector paths

- Status: **Proposed** (2026-11-24). Only the owner moves it to Accepted. No spike code has landed, so
  the evidence table below is incomplete on purpose: see "Evidence required before Accepted".
- Date: 2026-11-24
- Amends: [ADR-057](ADR-057-vector-paths-thorvg-default-and-exit.md) (its section 3 **Cost** trigger
  fires for the large-raster use case) and [ADR-004](ADR-004-buy-vector-path-rasterization.md).
- Related: issue #683, #681 and #697 (the upload measurements),
  [ADR-068](ADR-068-clip-state-in-a-storage-buffer.md) (RHI storage buffers), `tests/gpu/batched2d/cpu_to_gpu_4k_measure.cpp`.
- Terms: a **tile rasterizer** splits the target into small tiles, lists the path segments that touch
  each tile, and computes each pixel's coverage inside its own tile; **exact-area coverage** is the
  analytic fraction of a pixel a path covers, with no sampling.

## Context

ThorVG is the vector rasterizer today (ADR-057). It rasterizes on the CPU into a buffer that the RHI
uploads as a texture. At 3840x2160 RGBA8 the cost scales with **pixel area**, and the upload is the
smaller share, so moving only the copy to the GPU cannot fix it: the raster has to leave the CPU.

| 4K step (median) | RX 5300M dGPU | Radeon iGPU | RTX 4060 box (Ryzen laptop CPU) |
|---|---|---|---|
| ThorVG raster, sparse scene (30 shapes), incl. clear | 14.9 ms | 13.9 ms | **5.2 ms** |
| ThorVG raster, dense scene (13 full-frame fills), incl. clear | 30.9 ms | 31.0 ms | **9.2 ms** |
| `Texture::write` 4K, CPU side | 4.3 ms | 6.0 ms | 1.2 ms |
| animated frame, sparse / dense | 19.2 / 35.2 ms | 20.0 / 37.0 ms | 6.4 / 10.4 ms |
| budget at 60 / 120 / 240 Hz | 16.7 / 8.3 / 4.2 ms | same | same |

The third column was measured on 2026-11-24 with the same tool on a faster CPU, and it changes how
the trigger reads: the CPU raster is **CPU-bound**, so on a fast desktop-class CPU a 4K animated
frame fits 60 Hz (not 120 or 240 Hz), while on the two machines of the original measurement it does
not fit 60 Hz at all. The case for a GPU rasterizer is therefore "4K vector animation on mid-range
hardware, and any refresh rate above 60 Hz", not "4K vector animation is impossible".

The web is worse, not better: browser WebGPU `queue.writeTexture` of a 4K frame is 7.8 ms CPU side
and 29 ms until the GPU has the pixels (native Vulkan: 8.5 ms), so a CPU-rasterized 4K frame on the
web is raster plus 29 ms of upload latency.

## Decision (proposed)

**A GPU compute tile rasterizer (Vello-style), resolution independent.**

1. The path is encoded once on the CPU into a compact segment/command stream, cached by content hash
   and not by scale, and uploaded as a storage buffer.
2. Compute passes rasterize it: flatten (curves to lines at a device-space tolerance), bin, coarse
   tile lists, then fine per-tile exact-area analytic coverage (nonzero and even-odd), compositing
   into the frame's color target through the batched 2D renderer.
3. Cost scales with **segment count, not pixel area**. Scale, translate and animation change a
   transform, not an upload. There is no MSAA memory (a 4x MSAA 4K target alone is 133 MB).

Where encoding lives, decided per job:

| job | placement | why |
|---|---|---|
| validate, bounds, pack the stream | **CPU, incremental** | validation guards untrusted SVG; the CPU needs exact or conservative bounds for damage, scissor and hit testing without a GPU readback, which stalls the UI thread. Cost must track the changed range only (persistent mapped buffer, patched ranges) |
| transform, flatten, stroke expansion, dash, morph | **GPU** | an animation then changes an interpolation factor, not an encode. Variable-size output needs atomic counters, indirect dispatch and an overflow path that never blocks the UI thread |
| generating paths from data (chart series) | **conditional** | the data is CPU-owned, so this moves the generator and costs exact CPU bounds and determinism; only if the CPU encode exceeds budget |

CPU encode proxy (pack to 32-byte records, chunked bounds; excludes validation and upload, so a lower
bound): full re-encode 1k segments 0.017 ms, 100k 1.8-1.9 ms, 1M 20-23 ms; delta (1% dirty) 0.017 ms
at 1k and 100k, 0.17-0.20 ms at 1M. A delta encode fits the 0.5 ms budget; a full re-encode every
frame crosses it at about 28k segments. **Rule:** if the delta encode exceeds 0.5 ms per frame at a
realistic chart size, open a follow-up for GPU path generation; otherwise it stays out.

## RHI requirements

Compute pipelines and dispatch (and indirect dispatch), storage buffers (RHI 1.9, #669; ADR-068 added
`bind_storage_buffer`), storage-image write, buffer atomics, and barriers, each behind a capability
query. The RHI has **no compute pipeline or dispatch today**. Capability is answered by the device, never
inferred from the backend name (the rule ADR-049 set). WebGPU reports the same compute limits as
native Vulkan (1024 invocations, 32 KiB workgroup memory, 16 storage buffers per stage, timestamps,
subgroups), and Metal and lavapipe both provide compute.

## Determinism, goldens and fallbacks

- Goldens stay lavapipe-only. The exact-area arithmetic, the flatten tolerance and the comparison
  tolerance against ThorVG are defined by the spike; ThorVG stays as the **reference implementation**
  and as the SVG parser until an in-house parser exists.
- The null backend and `CpuCanvas` keep a CPU path. A device without compute keeps ThorVG raster plus
  upload, documented as the slow path.
- What moves to the GPU next on the same machinery: gradients, dash, clip-path polygon/path()
  (ADR-068, #673), conic gradients for dials (#617), glyph outlines.
- Per-frame budget gates (frame time and bytes uploaded) keep the claim measured.

## Alternatives, to be measured on the same 4K scenes

| option | expected problem | status |
|---|---|---|
| stencil-then-cover + MSAA | needs a stencil attachment the RHI lacks, 4x target memory, overdraw on big fills | not measured |
| Loop-Blinn / Slug-style fragment curve evaluation | strokes and overlapping fills still need a tessellator | not measured |
| ThorVG GL / WebGPU engines | no Vulkan engine exists, so not a Vulkan answer | rejected by inspection |
| keep ThorVG CPU raster + upload | meets 60 Hz at 4K on a fast CPU only | the baseline above |

A GPU throughput proxy (not a rasterizer): a 4K compute pass evaluating N analytic discs per pixel by
brute force costs 7.0 ms (N=16, min 3.1), 9.4 ms (N=64) and 37.4 ms (N=256) on the RX 5300M. Brute
force is not enough on a low-end dGPU, so the binned design is required, not optional.

## Evidence required before Accepted

The spike prototypes fill only, under a CMake option that is off by default and changes no default,
and renders the `sparse` and `dense` scenes of `cpu_to_gpu_4k_measure` at 4K.

| target (to be confirmed by the owner) | RX 5300M | iGPU | RTX 4060 | browser WebGPU |
|---|---|---|---|---|
| dense scene GPU time <= 4.7 ms (half the 64-disc proxy); sparse strictly lower; report min and p50 | open | open | open | open |
| CPU time per frame <= 0.5 ms and zero bytes uploaded per frame after the first | open | open | open | open |
| ThorVG comparison within the defined tolerance on the lavapipe golden | open | - | - | - |
| the alternatives above measured on the same scenes | open | open | open | open |

The proof that a compute rasterizer beats the CPU raster at 4K at all is the spike's own deliverable
and is unproven today. If the spike contradicts the proxy, this ADR is rejected with the numbers.

## Consequences

- Vector animation cost stops scaling with pixel area, and the web stops paying a 29 ms upload per frame.
- The RHI grows its first compute surface, which is also what glyph outlines, gradients and clip paths
  need next; that is the larger cost and the larger reuse.
- Two rasterizers exist for a while (ThorVG as reference and fallback), which doubles the golden surface
  until the tolerance is defined.
