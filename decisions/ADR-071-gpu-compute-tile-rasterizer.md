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

## Production pieces (2026-11-24)

The consumer-side groundwork is in the tree, behind `supports_compute()`, with no caller in the
renderer yet:

- `path_raster` in the shader manifest (compute stage, accepted alone; see
  [shader-manifest-v1.md](../docs/specs/shader-manifest-v1.md) section 8) and
  `src/rendering/backend/vulkan/shaders/path_raster.comp`, four passes selected by a define, exposed
  by `vulkan::path_raster_pipeline_desc(PathRasterPass)`. Every raster is a record of a `jobs` buffer
  (binding 6, `PathRasterJob`, 20 words): its sizes (including `out_stride`, so the buffer can be laid
  out for `copy_buffer_to_texture`, ADR-072) and the word offsets of its slices of the shared
  buffers. The push constant is one word (`job_base`). A batch of N rasters is one dispatch of each
  pass with the job as the second grid dimension (the fine pass: third).
- `path::gpu_scene_from_shapes`, `encode_gpu_scene`, `write_path_records`, `write_dash_offsets` and
  `plan_path_raster` (`include/tessera/rendering/path/gpu_path_*.hpp`): the CPU half, pure data and
  unit-tested without a device (`gpu_path_encoder`). The spike now links it, so its ThorVG
  comparison measures the production encoder (same diffs as before the move).
- `path_raster_gpu`: shapes -> encoder -> four passes -> `copy_buffer_to_texture` -> drawn texture,
  on a real device, with a padded copy pitch.
- `GpuPathRasterizer` (`include/tessera/rendering/path/gpu_path_rasterizer.hpp`): the recorder.
  `enqueue` encodes a raster on the CPU, `flush` packs the queued rasters into shared buffers and
  submits them in one command list without waiting (four dispatches and one copy per raster), `collect`
  returns the buffers and the pipeline set once the GPU is done. Buffers are pooled by capacity and
  bounded (`max_pooled_bytes`), a pipeline set is one per batch in flight (a compute pipeline owns its
  descriptor set, RHI 1.16, so a running batch's set cannot be rebound), and `warm()` builds sets at
  startup (a set is about 2 ms of pipeline creation). Test: `path_raster_batch_gpu`.
- `choose_raster_route` (`raster_route.hpp`): the CPU/GPU policy as a pure function (size line,
  bin-node bound), tested without a device.

Measured on the RX 5300M and the iGPU (2026-11-25, 32 rasters per flush, CPU encode, submit and wait
included): one raster per submission cost 0.70 ms each at 32 to 256 px; a pass of 8 rasters with four
dispatches each 0.15 ms (the split was 80% recording: 24 descriptor updates per raster, against 0.025
ms of encode and buffer creation); binding only the buffers a pass uses, a buffer pool and warm
slots, 0.10 ms; **one dispatch per pass for the whole batch, 0.040 ms at 32 px, 0.043 at 64 px and
0.059 at 256 px** (iGPU 0.042, 0.041, 0.092), of which the CPU's share (encode plus recording) is
0.027 ms per raster. ThorVG on the same CPU takes 0.036 ms at 32 and 64 px and 0.17 ms at 256 px, so a
batched GPU raster is about break-even at icon size on the clock and frees the CPU, and is 3x faster
at 256 px. The cost of reading the job record from a buffer instead of a push constant on the 4K
scenes is 2 to 8% on the RX 5300M and 3 to 4% on the iGPU (interleaved old/new runs; single runs of
this GPU drift by 30%, so compare interleaved).

`PathTextureCache` now has the GPU path (`set_gpu_rasterizer`, `flush_gpu`; test
`path_texture_cache_gpu`, `path_texture_cache` for the no-support case). A miss that
`choose_raster_route` sends to the GPU returns a texture created without pixels and queues its raster;
the frame calls `flush_gpu()` once after its last `acquire()` and before the draw list that samples
the textures, and `end_frame()` flushes whatever is left and collects finished batches. Any GPU
refusal (a dense raster, a full batch that will not flush, a device error) falls back to ThorVG for
that request and is counted (`gpu_fallbacks`); a failed flush drops the waiting entries so the next
`acquire` rasterizes them on the CPU. A texture evicted while its raster is still queued is submitted
first and kept one more frame, because the copy into it is in flight; device loss discards the queue.

No production code owns a `PathTextureCache` yet: the window device builds its sink without a
`VectorPaintConfig`, so SVG draws in a windowed app are not wired at all and only tests and tools
construct the cache. The composition-root recipe (build the rasterizer from
`vulkan::path_raster_pipeline_descs()`, `warm` at startup, `set_gpu_rasterizer`, `flush_gpu` before the
draw list, `end_frame` after) is on `path_raster_pipeline_descs` in
`include/tessera/rendering/vulkan/path_raster_pipeline.hpp`; whoever adds the windowed vector stack
follows it.

**The 128 px line stays** (`tests/gpu/batched2d/path_route_measure.cpp`, 48 distinct icons per
stream, best of 10, the four SVG fixtures at 32 to 256 px, ThorVG against the GPU route forced on,
per icon): the cost a frame pays is the main thread's (`acquire` plus `flush_gpu`). On the iGPU the
GPU route is at or below ThorVG from 32 px for the stroked icons (0.033 against 0.039 ms) and from
128 px for every fixture (0.043 against 0.045 to 0.12 ms), and 2 to 6 times cheaper at 256 px. On the
RX 5300M it is break-even to worse below 128 px (0.06 to 0.11 ms against 0.04 to 0.07), wins on the
complex icons from 128 px (0.077 against 0.10 ms; 0.114 against 0.21 at 192 px) and loses on the two
simple fixtures (one shape) up to 192 px, where ThorVG is still 0.05 to 0.08 ms. So the line moves
neither way on these numbers: ThorVG's cost grows with pixels times shapes, the GPU's mostly with
the batch, and a rule that also looks at the shape count would be the next refinement, with a real
icon set to measure it on (these fixtures are one or two shapes). One caution: the end-to-end time
(until the device is idle) on the RX 5300M is often 3 to 5 times its main-thread time and in some
runs sits at about 14 ms for a 48-icon batch against 3 to 5 ms in others, which looks like the
discrete GPU leaving a low power state, not our code, but it was not investigated; a batch of icons
can appear a frame or two later than a ThorVG one on that GPU.

Still open: the WebGPU port of the shader (it now binds seven storage buffers; WebGPU's default limit is eight per stage, so there is one
spare, and the port should check the device's limit).

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
| dense scene GPU time <= 4.7 ms (half the 64-disc proxy); sparse strictly lower; report min and p50 | **ring strokes: dense 5.61 / 5.66 ms (over the line), sparse 2.85 / 2.86 ms; GPU strokes: dense 8.77 / 8.82 ms (over), sparse 3.13 / 3.13 ms** (min / p50, 30 runs, 2026-11-25) | **ring strokes: dense 18.6 / 18.6 ms, sparse 8.45 / 8.45 ms; GPU strokes: dense 34.0 / 34.0 ms, sparse 10.1 / 10.1 ms** (both over the line) | **ring strokes: dense 3.22 / 3.22 ms, sparse 0.99 / 0.99 ms (2.53 / 2.56 and 0.92 / 0.92 for the first fill-only spike); GPU strokes: dense 4.07 / 4.09 ms, sparse 1.12 / 1.12 ms (min / p50, 20 runs)** | open |
| CPU time per frame <= 0.5 ms and zero bytes uploaded per frame after the first | **record+submit 0.32 ms p50 (met); 0 B per frame after 94 / 149 KB once; full re-encode 0.18 ms** | **record+submit 0.31 ms p50 (met); same bytes; re-encode 0.17 ms** | **record+submit 0.01 / 0.03 ms; full re-encode 0.09 / 0.11 ms; 0 B per frame after 93 / 149 KB once** | open |
| ThorVG comparison within the defined tolerance on the lavapipe golden | **lavapipe run (llvmpipe, LLVM 21.1.8): the pixels are identical to the RTX 4060's** (premultiplied diff max 57 sparse / 28 dense with ring strokes, 71 / 28 with GPU strokes; 0.49% / 0.37% of pixels off by >4, 0.28% / 0.013% by >16). The "defined tolerance" itself is still undefined; the golden image is not committed | - | - | - |
| the alternatives above measured on the same scenes | open | open | open | open |

Spike caveats (issue #683, `tools/poc/compute_raster`, RHI 1.16): the measured variant is a **scanline-binned exact-area** rasterizer (clear, bin per segment, fine per pixel), not the tile design above, and strokes are expanded on the GPU by a fourth pass (the CPU still flattens the curve to a centerline and uploads it once): each segment becomes a body quad plus a join wedge at its start and, on an open contour, a half-disc cap at its end, all wound the same way so signed areas cancel along the shared radii. Round, bevel and miter joins (with the SVG miter limit and its bevel fallback) and round, butt and square caps exist, and so do dashes (below). Per-path affine transforms exist (the 6 words of each path's record; a stroke is expanded in its own space and transformed when binned and shaded, so the pen scales with the path), which lets the "animation changes a factor, not an encode" claim be tested: an animated frame rewrites the path records only (13 / 30 / 9 paths = 832 / 1920 / 576 bytes, about 1 µs of CPU on the RTX 4060, against 0.13 / 0.16 / 0.007 ms for a full re-encode) and costs the GPU 4.54 / 1.10 / 0.74 ms p50 (dense / sparse / joins; the dense scene is 4.33 ms at best, inside the 4.7 ms line but with little room: the per-path affine lookup and the stroke-edge indirection cost the fill-heavy ring scene about 25% against the first fill-only spike, 3.22 ms against 2.56 ms, and skipping the multiply for identity paths bought nothing, so the cost is in the loads). Against ThorVG with the same transforms (rotate, scale 0.75 to 1.25, drift, a different transform per path) the frame matches as well as the static one: dense max 29, 0.26% off by >4; joins and caps max 58, 0.019%; fills under a non-uniform scale and shear max 60, 0.093% (ThorVG cannot stroke with a non-uniform pen, so strokes are compared under similarities only), identical on lavapipe. Two limits: the arc tessellation of round joins and caps is chosen once for the largest scale in the animation, and the bin-node capacity is sized for it, so an unbounded zoom would need a re-encode; and a transform that changes the box (the CPU rewrites each path's device-space box) is a per-path CPU cost that grows with the path count, not with the segment count.

Dashes are cut on the GPU too. The CPU splits each centerline segment into chunks no longer than one pattern period (once, independent of the offset), which bounds the dashes a chunk can hold to the on-entries of the pattern plus one, so each stroke record has a fixed number of piece slots; the stroke pass walks the pattern across its chunk from the phase `(path length + offset) mod period` and emits each dash as its own body, caps and (where a dash runs across a vertex) joins. The pattern lives in a per-path buffer of 12 words, so **marching the dashes is one float per dashed path**: 336 bytes and well under 1 µs of CPU for the 7 dashed paths of the dash scene, against 0.06 ms for a full re-encode, with the GPU at 1.57 ms p50 (1.52 min) for 792 records and 121k expanded edges at 4K. Against ThorVG the dash scene (butt, square and round caps; miter, bevel and round joins; an even, an odd, a four-entry and a 3 px pattern; an offset; a closed polygon; a circle) is off by max 22, 0.074% of pixels by >4 and 0.0003% by >16, and max 27 at frame 7 of the animation, identical on lavapipe; compared shape by shape every one is under max 21, the circle included (its dashes cross many short flattened segments, which showed the inner-join overdraw before the trim below). **The reference is a CPU dasher written in the spike**, because ThorVG's seam (`StrokeStyle`) has no dash: the same dashes cut on the CPU and stroked by ThorVG as separate open polylines. It tests the GPU's phase and cap logic against an independent stroker, not against an independent dasher. Not done: a dash pattern longer than 8 entries, and a pattern phase kept in f32 along a path (a very long path loses sub-pixel phase). The expansion costs the dense scene +0.9 ms over CPU rings (4.09 vs 3.22 ms with the wedge's arc edges proportional to the turn angle; with a fixed-size fan it was 5.2 ms, over the 4.7 ms line) and the sparse one +0.2 ms, in exchange for open polylines, which match ThorVG to max 28, 0.010% of pixels off by >4 (4 polylines, 9 to 40 px wide, acute and reversing turns). Miter and bevel joins, butt and square caps and closed polygons (9 strokes, miter limits 4 and 1.2, 10 to 30 px) match to max 8, 0.019% off by >4, none by >16 on both the RTX 4060 and lavapipe, at 0.59 ms. The inside of a join used to overcount: the two body quads overlap and their partial-coverage pixels add (max 33 here, 71 on the sparse scene, up to 119 on a rounded SVG icon). Each body is now cut where it meets the previous one (it starts on the previous body's end edge and follows the previous body's inner side to the point where the two inner offset lines meet), valid while that point lies inside both runs, which a dashed run shortens to where its dash began; otherwise the overlap is clamped as before. The cost is 3 more edge slots per record. **The SVG route** (`--svg-dir`): the production loader decodes a document (ThorVG is the parser; its geometry is what the texture cache rasterizes today), the spike flattens the shapes (Wang's bound, 0.05 px), encodes each fill and each stroke (every subpath its own contour, with its own caps and joins) and rasterizes them at 32 / 64 / 256 / 1024 px, against ThorVG on the same shapes. The 6 icons of the golden set and the 4 loadable fixtures (and the designer's phone frame) all rasterize. At 1024 px: GPU 0.31 ms, 0.35 ms with the CPU encode, ThorVG 0.56 ms; at 256 px: 0.06 / 0.10 / 0.07 ms; at 64 px: 0.024 / 0.067 / 0.022 ms; at 32 px: 0.022 / 0.065 / 0.027 ms. **For icon-sized textures the GPU is not faster than ThorVG**: the dispatch and the wait cost ~0.04 ms by themselves, and an icon is cached once per scale step. It wins from about 1000 px up, where the CPU raster is the cost. Pixels: at 256 px the mean fraction off by >4 is 0.73% and by >16 is 0.49% over the golden set, dominated by `chart.svg` (4 axis-aligned rectangles on fractional pixel edges, 1.2%): ThorVG snaps an axis-aligned rectangle to whole pixels, where this rasterizer anti-aliases the exact area, so the two differ by up to a third of the coverage along those edges and the GPU side is the exact one. The stroked icons are within max 38 (search at 256 px). Out of scope for this route: the document's colours reach the rasterizer through `PathShape` as they do today (solid fill and stroke only; gradients, masks and clips are rejected by the loader), a zero-length subpath (a round-cap dot) draws nothing, and the output is a storage buffer of straight RGBA8: **handing it to a texture needs an RHI addition** (a buffer-to-texture copy, or a storage-image output), which the RHI does not have at 1.16 and which a readback would replace with a blocking wait the UI thread may not take. That addition, the shader moving into the production shader manifest, and a `PathTextureCache` entry that rasterizes on the GPU are the remaining work before this can replace the CPU path for large vector surfaces.

**Measured on 2026-11-25 on a Ryzen 5 4600H laptop (MSVC Release, 4K, 30 runs) with the same tool, on the RX 5300M and the Radeon iGPU (`VK_LOADER_DEVICE_ID_FILTER=0x1636`):** against ThorVG on that CPU (sparse 11.4 ms, dense 24.8 ms), the RX 5300M wins every scene (sparse 2.9 / 3.1 ms, dense 5.7 / 8.8 ms for ring / GPU strokes; dashes 3.6 ms against 12.6; polylines 1.5 against 8.6; animated dense 9.9 ms, sparse 4.1 ms) but misses the 4.7 ms dense line, and with GPU strokes the 8.3 ms (120 Hz) budget too, so on mid-range hardware the dense scene fits 60 Hz, not 120 Hz. The iGPU is break-even at best: sparse 8.5 / 10.1 ms against 10.8 ms, dense 18.6 ms with ring strokes against 24.8 ms, and **slower than ThorVG on dense GPU strokes (34.0 against 23.4 ms)**; dashes 13.1 against 12.6 ms. The per-frame animation cost is the same as on the RTX 4060 (0.002 to 0.012 ms CPU, 336 to 1920 bytes). Pixels are identical to the RTX 4060 (same diffs against ThorVG). SVG route over the 4 loadable fixtures in `tests/fixtures/svg` (not the 6-icon golden set), GPU / ThorVG: RX 5300M 0.027 / 0.036 ms at 32 px, 0.040 / 0.036 at 64, 0.114 / 0.173 at 256, 0.328 / 2.32 at 1024; iGPU 0.018 / 0.031, 0.023 / 0.028, 0.067 / 0.178, 0.376 / 2.26. With the CPU encode and the wait (~0.09 to 0.13 ms wall) the GPU is slower than ThorVG up to about 64 px and wins from 256 px, so the crossover is lower on these GPUs than the ~1000 px seen on the RTX 4060 (those icons were simpler, so read it as a trend, not a threshold). Both GPUs are 2019 to 2020 parts, a lower bound for what the policy has to handle. The compute pipeline fallback to the CPU raster (the cache policy) is therefore needed for the dense, stroke-heavy case on weak GPUs. AMD discrete and iGPU are now measured; browser WebGPU stays open. The ThorVG diff is dominated by stroke edge geometry. Status stays Proposed: the owner decides.

The proof that a compute rasterizer beats the CPU raster at 4K at all is the spike's own deliverable
and is unproven today. If the spike contradicts the proxy, this ADR is rejected with the numbers.

## Consequences

- Vector animation cost stops scaling with pixel area, and the web stops paying a 29 ms upload per frame.
- The RHI grows its first compute surface, which is also what glyph outlines, gradients and clip paths
  need next; that is the larger cost and the larger reuse.
- Two rasterizers exist for a while (ThorVG as reference and fallback), which doubles the golden surface
  until the tolerance is defined.
