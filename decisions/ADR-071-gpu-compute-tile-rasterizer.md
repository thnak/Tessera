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

The spike prototypes fills and round-join/round-cap strokes, under a CMake option that is off by
default and changes no default, and renders the `sparse` and `dense` scenes of
`cpu_to_gpu_4k_measure` at 4K, plus an open-polyline scene for the strokes a ring cannot express.

| target (to be confirmed by the owner) | RX 5300M | iGPU | RTX 4060 | browser WebGPU |
|---|---|---|---|---|
| dense scene GPU time <= 4.7 ms (half the 64-disc proxy); sparse strictly lower; report min and p50 | open | open | **ring strokes: dense 2.53 / 2.56 ms, sparse 0.92 / 0.92 ms; GPU strokes: dense 3.55 / 3.57 ms, sparse 1.12 / 1.12 ms (min / p50, 20 runs)** | open |
| CPU time per frame <= 0.5 ms and zero bytes uploaded per frame after the first | open | open | **record+submit 0.01 / 0.03 ms; full re-encode 0.09 / 0.11 ms; 0 B per frame after 93 / 149 KB once** | open |
| ThorVG comparison within the defined tolerance on the lavapipe golden | **lavapipe run (llvmpipe, LLVM 21.1.8): the pixels are identical to the RTX 4060's** (premultiplied diff max 57 sparse / 28 dense with ring strokes, 71 / 28 with GPU strokes; 0.49% / 0.37% of pixels off by >4, 0.28% / 0.013% by >16). The "defined tolerance" itself is still undefined; the golden image is not committed | - | - | - |
| the alternatives above measured on the same scenes | open | open | open | open |

Spike caveats (issue #683, `tools/poc/compute_raster`, RHI 1.16): the measured variant is a **scanline-binned exact-area** rasterizer (clear, bin per segment, fine per pixel), not the tile design above, and strokes are expanded on the GPU by a fourth pass (the CPU still flattens the curve to a centerline and uploads it once): each segment becomes a body quad plus a join wedge at its start and, on an open contour, a half-disc cap at its end, all wound the same way so signed areas cancel along the shared radii. Round, bevel and miter joins (with the SVG miter limit and its bevel fallback) and round, butt and square caps exist, and so do dashes (below). Per-path affine transforms exist (the 6 words of each path's record; a stroke is expanded in its own space and transformed when binned and shaded, so the pen scales with the path), which lets the "animation changes a factor, not an encode" claim be tested: an animated frame rewrites the path records only (13 / 30 / 9 paths = 832 / 1920 / 576 bytes, about 1 µs of CPU on the RTX 4060, against 0.09 / 0.07 / 0.005 ms for a full re-encode) and costs the GPU 4.49 / 1.09 / 0.74 ms p50 (dense / sparse / joins; the dense scene is 4.20 ms at best, inside the 4.7 ms line but with little room, and the transform lookup costs about 10% on the static frame). Against ThorVG with the same transforms (rotate, scale 0.75 to 1.25, drift, a different transform per path) the frame matches as well as the static one: dense max 36, 0.26% off by >4; joins and caps max 60, 0.019%; fills under a non-uniform scale and shear max 60, 0.093% (ThorVG cannot stroke with a non-uniform pen, so strokes are compared under similarities only), identical on lavapipe. Two limits: the arc tessellation of round joins and caps is chosen once for the largest scale in the animation, and the bin-node capacity is sized for it, so an unbounded zoom would need a re-encode; and a transform that changes the box (the CPU rewrites each path's device-space box) is a per-path CPU cost that grows with the path count, not with the segment count.

Dashes are cut on the GPU too. The CPU splits each centerline segment into chunks no longer than one pattern period (once, independent of the offset), which bounds the dashes a chunk can hold to the on-entries of the pattern plus one, so each stroke record has a fixed number of piece slots; the stroke pass walks the pattern across its chunk from the phase `(path length + offset) mod period` and emits each dash as its own body, caps and (where a dash runs across a vertex) joins. The pattern lives in a per-path buffer of 12 words, so **marching the dashes is one float per dashed path**: 336 bytes and well under 1 µs of CPU for the 7 dashed paths of the dash scene, against 0.06 ms for a full re-encode, with the GPU at 1.57 ms p50 (1.52 min) for 792 records and 121k expanded edges at 4K. Against ThorVG the dash scene (butt, square and round caps; miter, bevel and round joins; an even, an odd, a four-entry and a 3 px pattern; an offset; a closed polygon; a circle) is off by max 81, 0.075% of pixels by >4 and 0.0009% by >16, the same at frame 7 of the animation, identical on lavapipe; compared shape by shape every one is under max 21 except the circle (max 81, 0.0007% by >16), whose dashes cross many short flattened segments and show the inner-join overdraw above. **The reference is a CPU dasher written in the spike**, because ThorVG's seam (`StrokeStyle`) has no dash: the same dashes cut on the CPU and stroked by ThorVG as separate open polylines. It tests the GPU's phase and cap logic against an independent stroker, not against an independent dasher. Not done: a dash pattern longer than 8 entries, and a pattern phase kept in f32 along a path (a very long path loses sub-pixel phase). The expansion costs the dense scene +0.7 ms over CPU rings (3.57 vs 2.89 ms with the wedge's arc edges proportional to the turn angle; with a fixed-size fan it was 5.2 ms, over the 4.7 ms line) and the sparse one +0.2 ms, in exchange for open polylines, which match ThorVG to max 28, 0.010% of pixels off by >4 (4 polylines, 9 to 40 px wide, acute and reversing turns). Miter and bevel joins, butt and square caps and closed polygons (9 strokes, miter limits 4 and 1.2, 10 to 30 px) match to max 33, 0.019% off by >4, 0.0001% by >16 on both the RTX 4060 and lavapipe, at 0.55 ms. A known flaw: on the inside of a join the two body quads overlap and their partial-coverage pixels add, so a few edge pixels come out too dark (max diff 71 against 57 for rings on the sparse scene; 0.50% vs 0.49% of pixels off by >4). Only the RTX 4060 is measured (no other GPU is available); AMD, iGPU and browser WebGPU stay open. The ThorVG diff is dominated by stroke edge geometry. Status stays Proposed: the owner decides.

The proof that a compute rasterizer beats the CPU raster at 4K at all is the spike's own deliverable
and is unproven today. If the spike contradicts the proxy, this ADR is rejected with the numbers.

## Consequences

- Vector animation cost stops scaling with pixel area, and the web stops paying a 29 ms upload per frame.
- The RHI grows its first compute surface, which is also what glyph outlines, gradients and clip paths
  need next; that is the larger cost and the larger reuse.
- Two rasterizers exist for a while (ThorVG as reference and fallback), which doubles the golden surface
  until the tolerance is defined.
