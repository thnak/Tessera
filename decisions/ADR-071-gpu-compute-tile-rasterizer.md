# ADR-071: A GPU compute tile rasterizer for vector paths

- Status: **Accepted with conditions** (2026-11-26, owner decision). Conditions: browser WebGPU numbers
  (recorded 2026-11-27 in Chrome, see "WebGPU numbers (Dawn)", Chrome subsection; the dense scene does
  not meet the line) and the comparison tolerance against ThorVG is defined before #685 ships; the CPU
  raster stays the fallback for icon-sized and weak-GPU cases. Proposed on 2026-11-24. The spike
  (`tools/poc/compute_raster`, #683) and the production pieces have landed; the evidence table below
  holds the measurements and lists what is still open (the alternatives, the
  tolerance against ThorVG, the dense-scene WebGPU gap).
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

| 4K step (median) | RX 5300M dGPU | Radeon iGPU | RTX 4060 desktop (Ryzen 5 9600X) |
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

## Corpus, ThorVG comparison and goldens (#685)

The committed corpus lives in code: [`path_raster_corpus.hpp`](../tests/gpu/batched2d/path_raster_corpus.hpp),
27 scenes of 96 x 96 px built from `GpuPathShape` values. It covers both fill rules (self-intersecting
star, nested contours with the same and opposite winding, alpha overlap, a fractional axis-aligned
rectangle), every cap (butt, round, square), every join (miter, bevel, round) and the miter-limit
bevel fallback, closed against open contours, dashes (even array, odd array with offset, closed
polygon, curve), and the degenerate cases (zero-length segment inside a line and alone, collinear
points, a sliver and a hairline, an empty scene, a shape fully outside the target, a shape clipped
by the target edge). Two tests iterate it:

- `path_raster_thorvg_compare` rasterizes every scene with the GPU passes and with ThorVG and diffs
  them with `rendering::golden::compare_rgba` on premultiplied channels. ThorVG has no dash in the
  seam, so a dashed scene's reference is the same dashes cut on the CPU (`path_raster_reference.hpp`)
  and stroked as open sub-paths, as in the spike; every scene is expressible. The tolerance table is in
  the test, one row per scene with the reason. Measured on lavapipe (llvmpipe, LLVM 21.1.8) as the
  largest per-channel delta of 255 and the pixels over 4 / over 16 of 9216, with the enforced limit
  (max / over 4 / over 16 pixels = measurement plus 25% or 2, counts x 1.5 + 1, a measured 0 stays 0).
  Run `path_raster_thorvg_compare_tests --report` to print the measurements. It skips without Vulkan,
  compute or ThorVG.
- `path_raster_golden` renders every scene twice (byte-identical or it fails), then compares bit for bit
  with [`tests/golden/gpu/path_raster/<scene>.png`](../tests/golden/gpu/path_raster) (straight-alpha
  RGBA, the rasterizer's own output). It skips on a hardware device and registers on the lavapipe lane
  only. Regenerate on a software device with `path_raster_golden_tests --update-goldens`, review the
  PNGs, commit.

| Scene | max | over 4 | over 16 | limit |
| --- | --- | --- | --- | --- |
| fill_star_non_zero | 2 | 0 | 0 | 4 / 0 / 0 |
| fill_star_even_odd | 2 | 0 | 0 | 4 / 0 / 0 |
| fill_nested_same_non_zero | 2 | 0 | 0 | 4 / 0 / 0 |
| fill_nested_same_even_odd | 2 | 0 | 0 | 4 / 0 / 0 |
| fill_nested_opposite_non_zero | 2 | 0 | 0 | 4 / 0 / 0 |
| fill_circle_alpha_overlap | 2 | 0 | 0 | 4 / 0 / 0 |
| fill_rect_fractional | 159 | 256 | 256 | 199 / 385 / 385 |
| stroke_cap_butt | 20 | 37 | 7 | 25 / 57 / 12 |
| stroke_cap_round | 24 | 99 | 27 | 30 / 150 / 42 |
| stroke_cap_square | 20 | 32 | 7 | 25 / 49 / 12 |
| stroke_join_miter | 6 | 16 | 0 | 8 / 25 / 0 |
| stroke_join_bevel | 6 | 21 | 0 | 8 / 33 / 0 |
| stroke_join_round | 16 | 57 | 0 | 20 / 87 / 0 |
| stroke_miter_exceeds_limit | 5 | 6 | 0 | 7 / 10 / 0 |
| stroke_closed_triangle_miter | 5 | 12 | 0 | 7 / 19 / 0 |
| stroke_open_triangle_butt | 5 | 2 | 0 | 7 / 4 / 0 |
| dash_even_butt | 5 | 2 | 0 | 7 / 4 / 0 |
| dash_odd_offset_round | 21 | 191 | 3 | 27 / 288 / 6 |
| dash_closed_polygon_butt | 52 | 18 | 2 | 65 / 28 / 4 |
| dash_curve_square | 62 | 44 | 3 | 78 / 67 / 6 |
| degenerate_zero_length_mid | 5 | 2 | 0 | 7 / 4 / 0 |
| degenerate_zero_length_dot | 12 | 21 | 0 | 15 / 32 / 0 |
| degenerate_collinear | 4 | 0 | 0 | 6 / 0 / 0 |
| degenerate_thin | 4 | 0 | 0 | 6 / 0 / 0 |
| degenerate_empty | 0 | 0 | 0 | 0 / 0 / 0 |
| degenerate_outside_target | 0 | 0 | 0 | 0 / 0 / 0 |
| degenerate_clipped_by_edge | 16 | 14 | 0 | 20 / 22 / 0 |

Reading the table: the fills agree to the coverage rounding of two anti-aliasers (2 of 255, no pixel
over 4). The strokes differ on edge pixels of joins, caps and dash ends (round caps and joins most:
arc tessellation). `fill_rect_fractional` differs by up to 159 by design: ThorVG snaps an axis-aligned
rectangle to whole pixels, the GPU keeps the coverage.

Two rows first recorded real GPU defects (issue #947): a zero-length segment inside a polyline
(`A, B, B, C`) lost the miter tip at `B` (32 pixels, up to 255) and a lone zero-length segment with a
round or square cap drew no dot (368 pixels). The stroke encoder now drops zero-length segments and
turns a contour that is nothing else into a 1/64 px segment (butt caps draw nothing), so both rows
sit at the normal stroke tolerance. `gpu_scene_from_shapes` already drops repeated points, so the
production path never produced these inputs; the scenes build `GpuPathShape` directly to pin the
kernels' behavior.

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

The windowed app owns one now (`src/app/window_vector.*`, `detail::VectorStack`): the
`WindowDevice` group builds it with the other per-device caches from `WindowOptions::vector_assets` /
`vector_bundles` (borrowed asset bundles; none means vector draws stay no-ops), owns the
`SvgDocumentCache`, a `PathTextureCache` of 256 entries / 64 MiB in the swapchain's format, and, when
the device can run it, a warmed `GpuPathRasterizer` routed by `choose_raster_route`. `render_and_present`
calls `flush()` after `paint_scene` and before the draw list is recorded, and `end_frame()` after the
submit, only once this window's older frame has finished (a texture evicted during a frame is not
destroyed while the frame before it may still sample it; with two windows sharing the cache the other
window's frame is not checked, so the cache is sized to keep a window's assets resident). A device
generation change releases and rebuilds the stack with the rest of the group's caches.
A project made by `tessera new` hands its generated `<ns>::gen::vector_assets::bundle()` to the window
and to `capture_frame` (the include is `__has_include`-guarded, so a module with no `Path` node still
builds); a capture rasterizes on ThorVG only, so its bytes stay golden-stable.
`tests/gpu/app_window_vector_tests.cpp` drives it on a real hidden window with validation on: 24
frames, each a new 140+ px raster, 24 GPU rasters, 0 fallbacks, validation-clean on both the RX 5300M
and the iGPU.

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

**Iconify check (2026-11-25).** The user's note: the 4K display overloads the RX 5300M, so its numbers
lean against the GPU route and the iGPU numbers are the fairer ones. Twelve icons from Iconify
(`https://api.iconify.design`, 24 px viewBox, one colour; mdi, lucide, tabler, ph, fa6-solid,
material-symbols, carbon, solar, fluent-emoji-flat, twemoji) through the same tool
(`path_route_measure <svg files>`), GPU total over ThorVG total on the iGPU, 32 to 256 px:
filled icons are 0.8 to 1.4 at 32 to 64 px and 0.3 to 0.9 from 128 px, so the 128 px line holds
for them (a filled mdi icon at 128 px: 0.07 ms on the GPU against 0.11 on ThorVG). Stroked icons
with round joins and caps (lucide, tabler) lose at every size, 1.4 to 3.4 times ThorVG, on both
GPUs. The cost follows the bin nodes, not the segment count or the empty slots: filled icons have 73 to
1,800 row nodes at 32 to 256 px, the lucide and tabler icons 4,500 to 81,000 (a round-join wedge per
flattened segment), and the row-node bound never tripped (262k). So the automatic route now also
bounds nodes per pixel (`max_nodes_per_kilopixel` = 250, `row_node_bound()`): a filled icon passes
(0.03 to 0.35 nodes per pixel), a stroked one does not (0.8 to 7.7). The line is inside the unmeasured
gap between 0.35 and 0.8 and sits on the CPU side. The pass split (`compute_raster_spike --passes N`, cumulative passes, iGPU, GPU timestamps) says the
fine pass is the cost: the lucide settings gear at 128 / 256 px takes 0.220 / 0.569 ms in the fine pass
against 0.032 / 0.044 for the stroke pass and 0.024 / 0.052 for bin; a filled gear of the same size
takes 0.024 / 0.058 in the fine pass. Cause: the fine pass is one thread per pixel and each thread
walks every node of its (path, row) list, so its cost is pixels times nodes per row (about 270 nodes
a row for the stroked icon against 4 for the filled one). Tried and dropped (2026-11-25, iGPU, the same gear at 128 px, GPU total, original 0.295 ms): a thread
per run of 8 pixels that walks each list once (edges left of the run add a carry, edges right of it
nothing, only overlapping edges pay for area) came out at 0.61 ms, twice as slow, with the same pixels;
running the walk as 4 interleaved chains (segment id modulo 4) changed nothing (0.61), calling the
exact area only for the columns an edge touches gave 0.57, and storing the transformed edge in the bin
node so the fine pass reads one record per node gave 0.54. The same code with one pixel per thread
(runs of 1) went back to 0.294. So the fine pass is not bound by its chain of dependent loads: it
needs the thousands of threads that hide the latency, and cutting the threads cut that faster than the
work. A real gain needs the node walk itself split across the threads of a group (a chain per lane
and a shared-memory reduction per path), which adds two barriers per path and would slow the 4K scenes
that have hundreds of paths; not worth it for the stroked icons, which the route keeps on ThorVG.

**The WGSL twin** (`src/rendering/backend/webgpu/shaders/path_raster.wgsl`, the `webgpu` source of the
manifest entry) carries the four passes as four entry points of one module. It differs from the GLSL
only where WGSL forces it: the work buffer is `array<atomic<u32>>` (every access is an
`atomicLoad`/`atomicStore`, relaxed, which is enough because each pass is its own dispatch), GLSL's
`mod` is written out (WGSL's `%` truncates), and the push constant is a uniform (binding 7 in the file as first written, 8 for the WebGPU
compute contract: `pc`). It binds seven storage buffers and one uniform; WebGPU's default limit is eight storage buffers
per stage, so there is one spare. The CI gate (`validate_wgsl.py`, naga 30.0.1) validates it. It was
also **run**: each entry point was translated to SPIR-V with naga, the entry renamed to `main`, and
the Vulkan tests and the 4K spike were rerun with those modules in place of the glslc ones (a
temporary hook, not kept). `path_raster_gpu`, `path_raster_batch_gpu` and `path_texture_cache_gpu` pass,
a control that swaps the fine pass for the clear pass fails all three, and all 43 comparison lines of the
spike against ThorVG (round, bevel and miter joins, the three caps, dashes, per-path transforms, the
animated frames) are identical to the digit with the GLSL. What is not shown: WebGPU itself (no
backend ran compute when this was written; see the WebGPU numbers below), and naga's
SPIR-V is not what a browser's compiler produces.

Still open: the frame code that owns a `GpuPathRasterizer` once windowed apps get a vector stack. The
WebGPU backend now embeds and runs the module (next subsection).

### WebGPU numbers (Dawn)

The module runs on the WebGPU backend (RHI 1.22 compute; `webgpu::path_raster_pipeline_descs()` and
`register_path_raster_shaders`, four programs of one WGSL source, one per entry point). The compute contract
([compute.md 3.1](../docs/design/gpu/compute.md)) fixed two edits to the WGSL: every storage buffer is
`read_write`, and the push block moved from binding 7 to 8. `GpuPathRasterizer` now binds all seven buffers for
every pass (a WebGPU bind group layout holds every slot; Vulkan ignores the extras), and the WebGPU backend
implements `copy_buffer_to_texture` (ADR-072 slice 3), so the production rasterizer runs unchanged.

**Where it ran, honestly:** `webgpu_path_raster_conformance` (Wasm, JSPI, run under Node 25.2.1 by
`scripts/webgpu_compute_conformance.mjs` with `TESSERA_CONFORMANCE_JS` pointing at it) on **Dawn-native through the
`webgpu` npm bindings, D3D12 backend, a real GPU adapter: NVIDIA GeForce RTX 4060 (Lovelace, driver 32.0.16.2140,
not a fallback adapter)**, 2026-11-27. It is **not a browser**: a page adds its own process split and
callback scheduling, so these numbers describe the backend on Dawn, not a page. Dawn's Vulkan backend did not start here (`vulkan-1.dll` failed to load in the Node
binding), so no Dawn-on-Vulkan comparison exists. No software adapter was used.

**Pixels.** The 27 corpus scenes were rendered by the WebGPU device and compared with the committed lavapipe
goldens, on premultiplied channels (a pixel whose alpha rounds to 0 carries no colour; the goldens keep the RGB
bytes of such a pixel, other devices do not), against the ThorVG limits of the table above. Largest premultiplied
channel delta per scene: **0 in 24 scenes and 1 in 3** (`fill_nested_same_non_zero`, `fill_nested_same_even_odd`,
`fill_nested_opposite_non_zero`, 2 pixels each); no pixel over 4 anywhere. 20 scenes are bit-identical to the
golden in straight alpha; the other 7 differ in 1 to 5 pixels (the 3 above, plus the RGB of alpha-0 pixels in
`stroke_cap_butt`, `stroke_cap_round`, `stroke_cap_square`, `stroke_join_miter`). Every scene is within its limit, with a
large margin. Rendering a scene twice gives the same bytes except `stroke_cap_square` (alpha-0 RGB again): the
node order of a row's list follows the atomic counter, so a GPU may sum a pixel's segments in another order
(lavapipe does not), which the test accepts up to 1 premultiplied. The reference is lavapipe, not ThorVG
(ThorVG is not part of the Wasm build), so the margin above is the cross-device spread of the same algorithm,
not an independent check.

**Time, 4K animated zoom** (`--measure`, the scenes of `gpu_path_animated_measure`: 120 paths with round-cap strokes,
and `--dense`: 13 overlapping fills; target 3840 x 2160 texture, 60 frames after 4 warm-up frames; the native column
is the same tool on the same machine and GPU through Vulkan, run the same minute):

First measurement (the fine pass visiting every path; the Vulkan columns are the same tool the same minute):

| | WebGPU sparse | WebGPU dense | Vulkan sparse | Vulkan dense |
|---|---|---|---|---|
| CPU enqueue + flush (min / p50 / max, us) | 287 / 513 / 889 | 398 / 541 / 804 | 15 / 44 / 88 | 12 / 41 / 79 |
| until the queue reported done (p50, ms) | 14.8 | 36.0 | 4.5 | 4.2 |
| 16 frames submitted, one wait: per frame (p50, ms) | 8.7 | 29.1 | n/a | n/a |
| empty-queue wait, the host's floor (p50, ms) | 15.4 | 15.5 | n/a | n/a |
| bytes the CPU wrote per frame | 7760 | 912 | 7760 | 912 |

**After the per-tile path mask** (commit f6218b58 for the GLSL, ported to `fine_main`: `var<workgroup>
tile_mask: array<atomic<u32>, 8>`, chunks of 256 paths, `atomicOr`, ascending-bit walk, no early return before the
barriers, x-range early-outs in `segment_area`, `node == kNil` skip). WGSL needs the barriers in uniform control
flow and the path count comes from a storage buffer, so invocation 0 stores it in a `var<workgroup>`, and every
invocation reads it back with `workgroupUniformLoad` for the chunk loop bound. Dawn accepted the module. Pixels:
the 27 scenes are unchanged against the lavapipe goldens (max premultiplied delta 0 in 24, 1 in the same 3, none
over 4; same 20 bit-identical). Same host, same session:

| | WebGPU sparse | WebGPU dense | Vulkan sparse | Vulkan dense |
|---|---|---|---|---|
| CPU enqueue + flush p50 (us) | 500 | 536 | 42 | 43 |
| until done p50 (ms) | 14.9 (floor 15.5) | 30.5 (floor 15.5) | 1.55 | 4.75 |
| 16 frames, one wait, per frame p50 (ms) | **5.8** (was 8.7) | **29.1** (was 29.1) | n/a | n/a |

The mask helps WebGPU's sparse scene (8.7 to 5.8 ms per frame) and does nothing for the dense one, the same shape
as on Vulkan (sparse 3.84 to 1.55 ms; dense 4.2 to 4.75 ms here, within this machine's run-to-run spread of the
dense scene, 4.12 ms in the coordinator's run). The dense scene is the 13 full-frame fills, where every tile
overlaps every path, so the mask removes nothing and WebGPU stays about 7x behind Vulkan there. Sparse is now
about 3.7x Vulkan's time.

Reading it: the first GPU row cannot be read as GPU time on WebGPU. Dawn-node resolves a completion about every
15.5 ms (an empty queue takes 15.4 ms at the median), so the sparse figure is the polling floor and the dense one
is that plus the work. The burst row pays the wait once for 16 frames and is the better estimate: **the sparse
frame costs about 8.7 ms and the dense frame about 29 ms, against 4.5 and 4.2 ms on Vulkan on the same GPU**, so
WebGPU on D3D12 is about 2x slower on sparse and **about 7x slower on dense, which misses the 4.7 ms line by far**
(the burst number includes the enqueue and flush, ~0.5 ms, and is not a pure GPU time; a
timestamp query would be, and is not wired). The CPU cost per frame is **0.5 ms, 12x the Vulkan cost** (0.04 ms),
and sits at the 0.5 ms line of the evidence table: each frame builds bind groups, writes four push slices and
encodes four passes through the JS bindings. Bytes written per frame equal Vulkan's (7760 and 912: the job
record and the per-path records, the document stays resident). The pool reuses buffers in steady state (4
created for the sequential run), and giving the pool room for 16 flights changed nothing, so the gap is not
allocation. Dawn's `disable_robustness` and `disable_workgroup_init` toggles changed nothing either (one run each,
dense); the cause of the dense gap is not found. A page may differ in either direction.

Consequence for the evidence table (Dawn-node numbers; Chrome follows): the browser column is no longer empty, but it is **not met** for the dense
scene on this stack, and the CPU line is met only at the limit. A page is measured in the next subsection; Dawn on Vulkan or Metal is not.

#### Chrome

The same Wasm program, built for a page (`webgpu_path_raster_web`: `-sENVIRONMENT=web -sJSPI`, the goldens
preloaded at `/goldens`), run in **Google Chrome 158.0.8127.38 (HeadlessChrome, `--headless=new
--enable-unsafe-webgpu`, default GPU flags, Windows 11)** on the same machine, 2026-11-27, driven by
`tools/webgpu_browser/run_path_raster.mjs` (a local server on 127.0.0.1, `tools/webgpu_browser/page.html` hands the
page's `navigator.gpu` device to `Module.preinitializedWebGPUDevice`, requested with the adapter's own
`maxStorageBufferBindingSize` and `maxBufferSize`, and POSTs what the program printed). JSPI was available with no
flag. Adapter as Chrome reports it: vendor `nvidia`, architecture `lovelace`, `isFallbackAdapter` false;
device and description are empty (Chrome withholds them), so that it is the RTX 4060 is inferred from the
machine having one GPU adapter, and the backend (D3D12 is Chrome's default on Windows) was not read out. The page
is cross-origin isolated (COOP/COEP headers) so `performance.now` has 5 us resolution instead of 100 us.
**Headless, not a visible window**, with background-timer throttling flags off; nothing is presented, so this
measures compute and the buffer-to-texture copy, not a frame on screen.

**Pixels.** Identical to the Dawn-node run, scene by scene: 27 scenes, max premultiplied delta 0 in 24 and 1 in the
3 `fill_nested_*` scenes (2 pixels each), none over 4, none over its limit; 20 bit-identical to the golden, one
scene (`stroke_cap_square`) not byte-identical between two renders (alpha-0 RGB). No validation errors, device
losses or warnings reached the page's error handler in any of the runs (corpus, sparse, dense).

**Time, 4K animated zoom** (60 frames after 4 warm-up frames; rows as in the Dawn tables, after the per-tile path
mask; Dawn-node and Vulkan columns repeated from above):

| | Chrome sparse | Chrome dense | Dawn-node sparse | Dawn-node dense | Vulkan sparse | Vulkan dense |
|---|---|---|---|---|---|---|
| CPU enqueue + flush (min / p50 / max, us) | 180 / 215 / 420 | 125 / 160 / 620 | ~313 / 500 / 699 | 363 / 536 / 863 | 13 / 42 / 64 | 21 / 43 / 76 |
| until the queue reported done (p50, ms) | 9.7 | 34.4 | 14.9 | 30.5 | 1.55 | 4.75 |
| 16 frames, one wait, per frame (p50, ms) | **6.1** | **30.0** | 5.8 | 29.1 | n/a | n/a |
| empty-queue wait (p50, ms) | 4.5 | 5.0 | 15.5 | 15.5 | n/a | n/a |
| bytes the CPU wrote per frame | 7760 | 912 | 7760 | 912 | 7760 | 912 |

Reading it. (1) **The GPU cost is the same as Dawn-node's**: 6.1 against 5.8 ms (sparse) and 30.0 against
29.1 ms (dense) per frame in the burst, so Chrome's WebGPU on this GPU shows the same ~7x gap to Vulkan on the
dense scene and ~4x on sparse; the dense scene misses the 4.7 ms line by a wide margin in a real browser too.
(2) **CPU per frame is about 0.2 ms** (2.5x better than Dawn-node's JS binding, 4-5x worse than native Vulkan),
inside the 0.5 ms line. (3) The empty-queue wait of 4.5 to 5 ms is mostly the program's own wait: it yields with
`emscripten_sleep(0)`, a `setTimeout(0)` that Chrome clamps to about 4 ms once nested, so the "until done" row
carries up to that much quantization and the burst row is the one to read (the first empty-queue wait is much
longer, hundreds of ms, from pipeline and device warm-up: the max column). (4) A headless run can differ from a
visible tab (GPU process scheduling, power state); it was not compared with a headed run, and the numbers are
from one run each of the commands in the table.

Consequence: the browser condition of this ADR is **recorded and the dense scene is not met** (Chrome and
Dawn-node agree). The CPU fallback for dense, stroke-heavy and icon-sized rasters stays required on the web;
the sparse scene is inside the 8.3 ms (120 Hz) budget (6.1 ms) and not inside 4.2 ms (240 Hz).

## Document residency and the 4K steady state (#685, 2026-11-27)

Three pieces take the CPU out of the frame. `GpuPathDocument` encodes geometry once in document
space (the scale is the per-path affine the shader already reads, so a resize re-encodes nothing; the
flatten tolerance and round-join arcs are sized for a power-of-two scale bucket, 1 to 32).
`GpuPathArena` keeps the fill edges, strokes and dashes in device buffers, uploaded once; a batch
whose rasters are all resident binds them and writes only its job record and 64 bytes per path (mixed
batches fall back to the per-flush copy). `PathTextureCache` acquires a document per geometry
identity and enqueues it with the raster scale.

`gpu_path_animated_measure` (RTX 4060, Windows, Release): 60 filled blobs and 60 round-cap strokes
(120 paths) into a 3840x2160 texture, the scale animating 1.90 to 2.00, 120 frames after 4 warm-up
frames.

| per frame | median | min | max |
|---|---|---|---|
| CPU, enqueue + flush (nothing waits) | **0.042 ms** | 0.015 | 0.083 |
| GPU until complete | 3.84 ms (**1.55 ms** after the per-tile path mask, f6218b58; min 1.53, max 1.94) | 3.78 | 4.75 |
| bytes written by the CPU | 7,760 | 7,760 | 7,760 |

The dense scene (`--dense`: 13 overlapping full-frame fills, 177 KB document) takes 0.041 ms of CPU,
912 bytes per frame and **4.23 ms** until the GPU is done (min 4.10, max 5.39; after the path mask 4.75 ms here, min 4.03, max 5.19, no gain: every tile overlaps every path, and 4.12 ms in another run): 0.06 ms over the
4.17 ms budget of 240 Hz, so "inside the 240 Hz budget on the dGPU" is **not** met by a hair on the
RTX 4060 at full 4K (the figure includes the 33 MB copy into the texture and the submit round trip;
the sparse scene is 3.84 ms). The second GPU of the manual gate has not been run here.

Against the ThorVG baseline above on the same machine (raster 5.2 ms, upload 1.2 ms CPU side, 33 MB
copied), the CPU cost drops by about 150x and the bytes by about 4,000x. The issue's "zero bytes
after the first frame" is not met literally: 7,760 bytes is the job record (80) plus 64 per path,
and it scales with the path count, not the pixel count. A frame whose transform did not change is a
texture cache hit and writes nothing. The arena took one upload and 123 hits; the pool reused 492
of 496 buffer requests only because the measurement raises `max_pooled_bytes` to 256 MiB: the
default 16 MiB bound destroys a 4K output buffer (33 MB, 64 MiB pooled) after every flush, so a
4K consumer must set the bound (or the default should follow the largest live target).

**Integration as built.** The app already routes vectors through this: `VectorStack`
(`src/app/window_vector.cpp`) owns the `GpuPathRasterizer` and attaches it to the
`PathTextureCache`, `PaintSink` vector draws and `Icon` acquire textures, and the GPU copy fills
them before the frame that samples them. The result is an ordinary textured quad, so clip state
(ADR-068), the sRGB/UNORM rule and the batched 2D renderer apply unchanged, and null and
`CpuCanvas` keep the ThorVG route. The seam of the issue's item 3 is `PathRenderer`
(`include/tessera/rendering/path/path_renderer.hpp`): asynchronous and batched (`enqueue` into a
texture the caller made, `flush` without waiting, `collect`), which the old `PathRasterizer`
contract (an RGBA8 buffer on the CPU) cannot express. `GpuPathRasterizer` implements it and
`PathTextureCache::set_path_renderer` takes any implementation. A renderer that blends straight
into the frame target (dropping the intermediate texture, with its own clip and blend handling) is
a possible second implementation; nothing measured needs it, so it is not built.
`VectorStack` raises the rasterizer's pool bound to 96 MiB so a 4K output buffer is kept.

**Where the dense 4K time goes (2026-11-27, RTX 4060, temporary shader experiments, not kept).** Dense
fine pass, 4.13 ms median: with `segment_area` replaced by a no-op 1.63 ms, and with the list walk
removed 0.80 ms (which still contains the 33 MB copy, about 0.55 ms). So the per-node coverage math is
about 2.5 ms, the list walk and transform about 0.85 ms, the rest copy, box tests and blend. The
sparse scene is the opposite (path loop, fixed by the tile mask). The next lever is to classify each
(row, path) list once per tile instead of once per pixel: segments wholly left of the tile add a
constant winding, wholly right add nothing, and only crossing segments need the per-pixel math,
which cuts the redundant work about 16x. It changes the summation order of the winding (rounding, so
goldens would be regenerated) and needs per-row shared lists with an overflow fallback in both the
GLSL and the WGSL; **It was built and measured, and it is slower:** four barriers per overlapping path (reset, classify,
total and compact, evaluate) plus a serial per-row compaction took the dense scene from 4.1 to 12.3 ms
and the sparse scene from 1.32 to 1.67 ms on the RTX 4060, so it was reverted. Barrier-heavy
per-tile cooperation does not pay here; a version that avoids the barriers (for example classifying in
the bin pass, where the segment order is already known) is the remaining idea, not built. Dense stays
at 4.10 to 4.16 ms against the 4.17 ms budget.

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
| dense scene GPU time <= 4.7 ms (half the 64-disc proxy); sparse strictly lower; report min and p50 | **ring strokes: dense 5.61 / 5.66 ms (over the line), sparse 2.85 / 2.86 ms; GPU strokes: dense 8.77 / 8.82 ms (over), sparse 3.13 / 3.13 ms** (min / p50, 30 runs, 2026-11-25) | **ring strokes: dense 18.6 / 18.6 ms, sparse 8.45 / 8.45 ms; GPU strokes: dense 34.0 / 34.0 ms, sparse 10.1 / 10.1 ms** (both over the line) | **ring strokes: dense 3.22 / 3.22 ms, sparse 0.99 / 0.99 ms (2.53 / 2.56 and 0.92 / 0.92 for the first fill-only spike); GPU strokes: dense 4.07 / 4.09 ms, sparse 1.12 / 1.12 ms (min / p50, 20 runs)** | **Chrome 158 headless, burst per frame: dense 30.0 ms (not met), sparse 6.1 ms; Dawn-node (not a browser): dense 29.1 ms (not met), sparse 8.7 ms; the sequential wait is 14.8 / 36.0 ms p50 but sits on a 15.4 ms host polling floor** (see WebGPU numbers) |
| CPU time per frame <= 0.5 ms and zero bytes uploaded per frame after the first | **record+submit 0.32 ms p50 (met); 0 B per frame after 94 / 149 KB once; full re-encode 0.18 ms** | **record+submit 0.31 ms p50 (met); same bytes; re-encode 0.17 ms** | **record+submit 0.01 / 0.03 ms; full re-encode 0.09 / 0.11 ms; 0 B per frame after 93 / 149 KB once** | **Chrome 158 headless: CPU 0.22 / 0.16 ms p50 (met); Dawn-node: 0.51 / 0.54 ms (at the line, 12x Vulkan); 7760 / 912 B per frame, document resident** |
| ThorVG comparison within the defined tolerance on the lavapipe golden | **lavapipe run (llvmpipe, LLVM 21.1.8): the pixels are identical to the RTX 4060's** (premultiplied diff max 57 sparse / 28 dense with ring strokes, 71 / 28 with GPU strokes; 0.49% / 0.37% of pixels off by >4, 0.28% / 0.013% by >16). The "defined tolerance" itself is still undefined; the golden image is not committed | - | - | **27 corpus scenes vs the lavapipe goldens, premultiplied: max delta 0 (24 scenes) or 1 (3), none over 4** |
| the alternatives above measured on the same scenes | open | open | open | open |

Spike caveats (issue #683, `tools/poc/compute_raster`, RHI 1.16): the measured variant is a **scanline-binned exact-area** rasterizer (clear, bin per segment, fine per pixel), not the tile design above, and strokes are expanded on the GPU by a fourth pass (the CPU still flattens the curve to a centerline and uploads it once): each segment becomes a body quad plus a join wedge at its start and, on an open contour, a half-disc cap at its end, all wound the same way so signed areas cancel along the shared radii. Round, bevel and miter joins (with the SVG miter limit and its bevel fallback) and round, butt and square caps exist, and so do dashes (below). Per-path affine transforms exist (the 6 words of each path's record; a stroke is expanded in its own space and transformed when binned and shaded, so the pen scales with the path), which lets the "animation changes a factor, not an encode" claim be tested: an animated frame rewrites the path records only (13 / 30 / 9 paths = 832 / 1920 / 576 bytes, about 1 µs of CPU on the RTX 4060, against 0.13 / 0.16 / 0.007 ms for a full re-encode) and costs the GPU 4.54 / 1.10 / 0.74 ms p50 (dense / sparse / joins; the dense scene is 4.33 ms at best, inside the 4.7 ms line but with little room: the per-path affine lookup and the stroke-edge indirection cost the fill-heavy ring scene about 25% against the first fill-only spike, 3.22 ms against 2.56 ms, and skipping the multiply for identity paths bought nothing, so the cost is in the loads). Against ThorVG with the same transforms (rotate, scale 0.75 to 1.25, drift, a different transform per path) the frame matches as well as the static one: dense max 29, 0.26% off by >4; joins and caps max 58, 0.019%; fills under a non-uniform scale and shear max 60, 0.093% (ThorVG cannot stroke with a non-uniform pen, so strokes are compared under similarities only), identical on lavapipe. Two limits: the arc tessellation of round joins and caps is chosen once for the largest scale in the animation, and the bin-node capacity is sized for it, so an unbounded zoom would need a re-encode; and a transform that changes the box (the CPU rewrites each path's device-space box) is a per-path CPU cost that grows with the path count, not with the segment count.

Dashes are cut on the GPU too. The CPU splits each centerline segment into chunks no longer than one pattern period (once, independent of the offset), which bounds the dashes a chunk can hold to the on-entries of the pattern plus one, so each stroke record has a fixed number of piece slots; the stroke pass walks the pattern across its chunk from the phase `(path length + offset) mod period` and emits each dash as its own body, caps and (where a dash runs across a vertex) joins. The pattern lives in a per-path buffer of 12 words, so **marching the dashes is one float per dashed path**: 336 bytes and well under 1 µs of CPU for the 7 dashed paths of the dash scene, against 0.06 ms for a full re-encode, with the GPU at 1.57 ms p50 (1.52 min) for 792 records and 121k expanded edges at 4K. Against ThorVG the dash scene (butt, square and round caps; miter, bevel and round joins; an even, an odd, a four-entry and a 3 px pattern; an offset; a closed polygon; a circle) is off by max 22, 0.074% of pixels by >4 and 0.0003% by >16, and max 27 at frame 7 of the animation, identical on lavapipe; compared shape by shape every one is under max 21, the circle included (its dashes cross many short flattened segments, which showed the inner-join overdraw before the trim below). **The reference is a CPU dasher written in the spike**, because ThorVG's seam (`StrokeStyle`) has no dash: the same dashes cut on the CPU and stroked by ThorVG as separate open polylines. It tests the GPU's phase and cap logic against an independent stroker, not against an independent dasher. Not done: a dash pattern longer than 8 entries, and a pattern phase kept in f32 along a path (a very long path loses sub-pixel phase). The expansion costs the dense scene +0.9 ms over CPU rings (4.09 vs 3.22 ms with the wedge's arc edges proportional to the turn angle; with a fixed-size fan it was 5.2 ms, over the 4.7 ms line) and the sparse one +0.2 ms, in exchange for open polylines, which match ThorVG to max 28, 0.010% of pixels off by >4 (4 polylines, 9 to 40 px wide, acute and reversing turns). Miter and bevel joins, butt and square caps and closed polygons (9 strokes, miter limits 4 and 1.2, 10 to 30 px) match to max 8, 0.019% off by >4, none by >16 on both the RTX 4060 and lavapipe, at 0.59 ms. The inside of a join used to overcount: the two body quads overlap and their partial-coverage pixels add (max 33 here, 71 on the sparse scene, up to 119 on a rounded SVG icon). Each body is now cut where it meets the previous one (it starts on the previous body's end edge and follows the previous body's inner side to the point where the two inner offset lines meet), valid while that point lies inside both runs, which a dashed run shortens to where its dash began; otherwise the overlap is clamped as before. The cost is 3 more edge slots per record. **The SVG route** (`--svg-dir`): the production loader decodes a document (ThorVG is the parser; its geometry is what the texture cache rasterizes today), the spike flattens the shapes (Wang's bound, 0.05 px), encodes each fill and each stroke (every subpath its own contour, with its own caps and joins) and rasterizes them at 32 / 64 / 256 / 1024 px, against ThorVG on the same shapes. The 6 icons of the golden set and the 4 loadable fixtures (and the designer's phone frame) all rasterize. At 1024 px: GPU 0.31 ms, 0.35 ms with the CPU encode, ThorVG 0.56 ms; at 256 px: 0.06 / 0.10 / 0.07 ms; at 64 px: 0.024 / 0.067 / 0.022 ms; at 32 px: 0.022 / 0.065 / 0.027 ms. **For icon-sized textures the GPU is not faster than ThorVG**: the dispatch and the wait cost ~0.04 ms by themselves, and an icon is cached once per scale step. It wins from about 1000 px up, where the CPU raster is the cost. Pixels: at 256 px the mean fraction off by >4 is 0.73% and by >16 is 0.49% over the golden set, dominated by `chart.svg` (4 axis-aligned rectangles on fractional pixel edges, 1.2%): ThorVG snaps an axis-aligned rectangle to whole pixels, where this rasterizer anti-aliases the exact area, so the two differ by up to a third of the coverage along those edges and the GPU side is the exact one. The stroked icons are within max 38 (search at 256 px). Out of scope for this route: the document's colours reach the rasterizer through `PathShape` as they do today (solid fill and stroke only; gradients, masks and clips are rejected by the loader), a zero-length subpath (a round-cap dot) draws nothing, and the output is a storage buffer of straight RGBA8: **handing it to a texture needs an RHI addition** (a buffer-to-texture copy, or a storage-image output), which the RHI does not have at 1.16 and which a readback would replace with a blocking wait the UI thread may not take. That addition, the shader moving into the production shader manifest, and a `PathTextureCache` entry that rasterizes on the GPU are the remaining work before this can replace the CPU path for large vector surfaces.

**Measured on 2026-11-25 on a Ryzen 5 4600H laptop (MSVC Release, 4K, 30 runs) with the same tool, on the RX 5300M and the Radeon iGPU (`VK_LOADER_DEVICE_ID_FILTER=0x1636`):** against ThorVG on that CPU (sparse 11.4 ms, dense 24.8 ms), the RX 5300M wins every scene (sparse 2.9 / 3.1 ms, dense 5.7 / 8.8 ms for ring / GPU strokes; dashes 3.6 ms against 12.6; polylines 1.5 against 8.6; animated dense 9.9 ms, sparse 4.1 ms) but misses the 4.7 ms dense line, and with GPU strokes the 8.3 ms (120 Hz) budget too, so on mid-range hardware the dense scene fits 60 Hz, not 120 Hz. The iGPU is break-even at best: sparse 8.5 / 10.1 ms against 10.8 ms, dense 18.6 ms with ring strokes against 24.8 ms, and **slower than ThorVG on dense GPU strokes (34.0 against 23.4 ms)**; dashes 13.1 against 12.6 ms. The per-frame animation cost is the same as on the RTX 4060 (0.002 to 0.012 ms CPU, 336 to 1920 bytes). Pixels are identical to the RTX 4060 (same diffs against ThorVG). SVG route over the 4 loadable fixtures in `tests/fixtures/svg` (not the 6-icon golden set), GPU / ThorVG: RX 5300M 0.027 / 0.036 ms at 32 px, 0.040 / 0.036 at 64, 0.114 / 0.173 at 256, 0.328 / 2.32 at 1024; iGPU 0.018 / 0.031, 0.023 / 0.028, 0.067 / 0.178, 0.376 / 2.26. With the CPU encode and the wait (~0.09 to 0.13 ms wall) the GPU is slower than ThorVG up to about 64 px and wins from 256 px, so the crossover is lower on these GPUs than the ~1000 px seen on the RTX 4060 (those icons were simpler, so read it as a trend, not a threshold). Both GPUs are 2019 to 2020 parts, a lower bound for what the policy has to handle. The compute pipeline fallback to the CPU raster (the cache policy) is therefore needed for the dense, stroke-heavy case on weak GPUs. AMD discrete and iGPU are now measured; browser WebGPU stays open. The ThorVG diff is dominated by stroke edge geometry. Status stays Proposed: the owner decides.

The spike answered the question the proxy left open: the compute rasterizer beats the CPU raster at 4K
on all three GPUs measured, with the two exceptions recorded above (the dense scene misses the 4.7 ms
line on the RX 5300M, and the iGPU loses to ThorVG on dense GPU strokes). The remaining measurements
are browser WebGPU, the alternatives table and the ThorVG tolerance.

## Consequences

- Vector animation cost stops scaling with pixel area, and the web stops paying a 29 ms upload per frame.
- The RHI grows its first compute surface, which is also what glyph outlines, gradients and clip paths
  need next; that is the larger cost and the larger reuse.
- Two rasterizers exist for a while (ThorVG as reference and fallback), which doubles the golden surface
  until the tolerance is defined.
