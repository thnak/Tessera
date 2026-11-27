# Offscreen layers and the render-pass break

Status: implemented in `src/rendering/gpu/` and RHI 1.1 for issue #242
([`glass-v1.md`](../../specs/glass-v1.md) section 4), and completed by the RHI 1.3 sampled view of a
render target (issue #254, [`rhi.md`](rhi.md) section 5.1). It is the seam `backdrop-filter` (#244), isolated layers
(#227), shared-element snapshots and stacking contexts build on. #244 landed the paint-side consumer
and the blur shaders on top of it; #254 landed the capability that makes the composite execute, so
what remains for #244 is wiring the two blur passes and the composite draw into the paint segment
(section 5.1) - not a new GPU capability. The RHI operation itself is normative in
[`rhi.md`](rhi.md) sections 5 and 5.1.

## 1. The problem

`Batched2DRenderer` draws one batch stream straight into the frame target and closes the render pass
once, in `end()`. Glass needs to read what the frame already holds at the moment a node is painted;
isolated layers need a subtree painted into its own texture and composited later. Neither is
expressible on a single unbroken pass: a render target cannot be sampled while it is a color
attachment, and "what was drawn before this node" is not a resource.

The seam adds exactly two capabilities and keeps every tree that does not use them on today's
single-pass path:

1. an RHI region copy between render targets (`CommandList::copy_target_region`), and
2. a render-pass break/resume in the batched renderer that keeps the frame target, plus a bounded
   pool of offscreen targets to break into.

## 2. RHI: `copy_target_region`

`include/tessera/rendering/rhi/device.hpp`:

```cpp
virtual void copy_target_region(const RenderTarget& source, const Scissor& source_region,
                                const RenderTarget& destination,
                                Offset2D destination_offset = {}) = 0;
```

- Integer pixel region, 1:1 copy (no scaling, no filtering, no format conversion). `Scissor` is the
  RHI's integer rectangle (`x`, `y`, `width`, `height`, half-open) and `Offset2D` the destination
  placement, so the operation composes with the damage/scissor vocabulary the renderer already uses.
- Both targets are non-presentable, 1-sample offscreen targets with the same format; `source` must
  have rendered content; source and destination must differ. Presentable (swapchain) images are
  rejected in RHI 1.1: reading one requires `TRANSFER_SRC` usage at swapchain creation, which is a
  window/swapchain decision ([`../window-resize.md`](../window-resize.md)). A window that wants the
  seam through the swapchain can either add that usage later or render into the scaled offscreen
  target it may already own (the adaptive render-scale path).
- The operation is legal only outside a render pass (the same rule `copy_target_to_buffer` and
  `blit_target` follow), so a renderer that needs to read the frame target ends the pass first.
- Invalid input throws (`std::invalid_argument` for region/format/sample/presentable/same-target, a
  pass-active error for order); nothing is silently clipped. Backends own the layout transitions;
  no barrier is exposed to callers.

Backend behavior:

| Backend | Implementation | Source layout | Destination end state |
|---|---|---|---|
| Vulkan | `vkCmdCopyImage` + `VkImageMemoryBarrier` | `TRANSFER_SRC_OPTIMAL` (what `end_render_pass` leaves an offscreen target) | `TRANSFER_SRC_OPTIMAL`, transfer write available to a transfer read |
| WebGPU | `wgpuCommandEncoderCopyTextureToTexture` | tracked by WebGPU | tracked by WebGPU |
| Null | recorded request (`NullTargetRegionCopy`) | n/a | n/a |

Because a copy destination must be created with transfer-dst usage, 1-sample Vulkan render targets
gain `VK_IMAGE_USAGE_TRANSFER_DST_BIT` and 1-sample WebGPU render targets `COPY_DST` (they already
had `TRANSFER_SRC`/`COPY_SRC` for readback). Multisampled color images keep their current usage set -
`copy_target_region` rejects them, and a 4x target's resolved image is only ever a readback source.

The Vulkan layout convention is the same one `end_render_pass`, `blit_target` and a later
`begin_render_pass(LoadOp::Load)` already use, so copies chain (copy -> copy -> readback) and a
pass can resume from the copied target's source after the copy.

## 3. Renderer: break and resume

`Batched2DRenderer` gained four members (see
[`batched2d_renderer.hpp`](../../../include/tessera/rendering/gpu/batched2d_renderer.hpp)):

```cpp
void break_pass();       // flush, end the pass; the frame target/desc stay current
void resume_pass();      // begin the same target with LoadOp::Load and rebind pipeline state
bool pass_broken() const;
rhi::RenderTarget* acquire_offscreen_target(rhi::Extent2D extent);
```

Contract:

- `break_pass()` flushes every pending quad first, so no batch is left to be emitted into a closed
  pass; it is idempotent and a no-op outside a frame. Draws must not be recorded while the pass is
  broken - only command-list operations (the copy into a pooled target).
- `resume_pass()` reopens `begin()`'s target with `LoadOp::Load`, so everything drawn before the
  break survives, then rebinds the pipeline, viewport, current scissor and folded clip uniforms. The
  renderer's own state (transform stack, scissor stack, clip stack, content scale, vertex buffer
  cursor) is untouched: geometry after the break continues the pre-break stream.
- `end()` on a broken frame resumes first, so the pass count stays balanced and the frame target
  ends in the same state as an unbroken frame.
- A tree that never breaks a pass records exactly the same commands as before the seam (one pass,
  same batches, same draws) - the existing goldens are byte-identical, and the null-backend test
  asserts the single-pass command stats explicitly.

## 4. Pooled offscreen targets

The pool (`include/tessera/rendering/gpu/offscreen_target_pool.hpp`) is bounded, recycled inside the
frame and across frames, and gives its memory back when idle (the numbers are in section 5.2, 5e):

- **Bound:** two hard caps, both on what is held at once. `Batched2DConfig::max_offscreen_targets` is
  `glassMaxBlurTargets` from [`glass-v1.md`](../../specs/glass-v1.md) section 5, default **4**, and
  0 disables the pool (`acquire_offscreen_target` returns null and the caller paints without the
  effect). `Batched2DConfig::max_offscreen_bytes` (default 256 MiB; 0 = none) caps the bytes of all
  pooled targets, retired ones included. A request over either cap returns null and is counted in
  `OffscreenPoolStats::exhausted`.
- **Release:** a caller that is finished with a target - its last pass and sampled bind are recorded -
  returns it with `release_offscreen_target` (the renderer does this for layer, mask, filter scratch
  and glass source/blur targets), and the pool hands it out again at once, in the same frame and in
  the next, at any `frames_in_flight`. The command list is sequential and the queue executes
  submissions in order, so the backend only has to order the new pass after the old reads
  (section 5e). The cap is therefore on the PEAK number of live targets, not the number of layers or
  the ring depth.
- **Retirement of an unreleased target:** a target a caller never releases (a bare
  `acquire_offscreen_target`) is referenced by its frame's command list, so it is handed out again
  only after `frames_in_flight` frame slots have rotated - the "frame F is complete" point the
  vertex-buffer slots already rely on. A target is only ever destroyed (grown, trimmed) after that
  rotation: when a free target that a frame in flight may still read is too small for a request, it is
  retired - kept alive outside the pool, charged to the byte cap - and a larger one takes its slot, so
  a live window resize does not starve the pool for `frames_in_flight` frames.
- **Reuse:** a steady-state screen holds the same targets for the whole run (the 300-frame null tests
  keep `offscreen_target_count()` at the peak live count at every ring depth, 1, 2 and 8, the window's).
  Consecutive acquires without a release return different targets, so a two-pass blur can ping-pong.
- **Sizing:** a free target grows to the largest extent requested on it and keeps that high-water
  extent. Targets use the frame target's format and a transparent-black clear.
- **Idle:** a target unused for `Batched2DConfig::offscreen_idle_trim_frames` frames (default 120, never
  fewer than `frames_in_flight`) is destroyed at the next `begin()`, and an idle driver calls
  `trim_offscreen_targets(true)` once its GPU work has drained, so a static UI holds no layer memory.
- **Allocation:** targets are created lazily and reused; the pool's slots are reserved at construction,
  so acquire, release, begin and stats allocate nothing in the steady state (a test replaces
  `operator new` and counts), and nothing blocks the UI thread.

## 5. Consumers

### 5.1 `backdrop-filter` (#244)

The paint pass splits the node list into segments at each glass node. At a segment boundary the
renderer breaks the pass, copies the node's box expanded by `3 * sigma` (clamped to the target) into
a pooled target, resumes, and draws the blurred copy clipped to the node's rounded box followed by
the node's own background/children. The blur itself (separable Gaussian, `glass_blur.frag`) is #244
and now runs in the renderer (below); this seam provides the copy, the break and the bounded target.

What #244 landed: the paint pass emits one `PaintSink::draw_backdrop` per glass node at exactly the
segment boundary (after everything painted earlier in paint order, before the node's own box
visuals), carrying the clamped sigma, the rounded composite clip, the folded node opacity and the
saturate/brightness/contrast factors. The style surface lives in
[`src/ui/style/backdrop_filter.cpp`](../../../src/ui/style/backdrop_filter.cpp): the grammar, the
`UnsupportedValue` diagnostic that drops the whole declaration, the `-webkit-` alias, the initial
`none` and `@supports`. High-contrast mode (`PaintContext::high_contrast`, glass-v1 section 3.5)
drops the draw at the paint pass and the opaque fallback paints; the flag and the two glass limits
are part of the paint-cache key, so a flip re-records instead of replaying a recorded backdrop.

`src/rendering/gpu/glass_blur.cpp` owns the shared decision - the 3 sigma dependency region,
`ceil(3 * sigma)` reach with `radius + 1` linear-sampling taps, the `glassMaxBlurRadius` clamp and
the half-resolution rule above `glassMaxBackdropArea` - so paint, damage and the shader cannot
disagree about the kernel. Damage that intersects a glass node's region adds its full box
(`DamageTracker::add_glass_dependencies`); a tree with no glass node returns before walking the
scene, and a static glass screen settles to zero damage and zero presents. `runtime::FrameDiagnostics`
has the `glass_ms` and `glass_backdrop_count` fields; the count comes from `GlassPaintStats`, and
`glass_ms` is the CPU record time of the backdrop draws, not a GPU interval
([`glass-v1.md`](../../specs/glass-v1.md) section 8.1). `glass_blur.frag`/`glass_blur.wgsl` implement
the two passes, the color matrix and the rounded composite clip.

The renderer wiring landed with #244 in `src/rendering/gpu/glass_compositor.cpp`
(`GlassCompositor::compose`, called by `Batched2DRenderer::draw_backdrop`), driven by
`Batched2DSink::draw_backdrop` on the real paint path:

1. `break_pass()` after flushing the vertex batch; the frame target stays current.
2. `plan_glass_backdrop_region` (clamped `3 * sigma` box, half resolution above the area limit)
   then two `acquire_offscreen_target` calls (source copy, blur target) from the bounded pool.
3. `copy_target_region(frame, region, source, 0)`, then the horizontal pass: `bind_pipeline(blur)`,
   `bind_sampled_target(source)`, `begin_render_pass(blur target)`, push constants (mode 0),
   `draw(3)`.
4. `bind_sampled_target(blur)`, `resume_pass()` (LoadOp::Load), `bind_pipeline(composite)`, scissor
   `intersect(frame scissor, region)`, push constants (mode 1: color matrix, rounded clip, opacity),
   `draw(3)`, then the renderer's frame state is rebound for the geometry that follows.

The two pipelines differ only in blend state: the horizontal pass writes straight-alpha blur data
into a pooled target (no blending), the composite draws over the frame target with alpha blending so
`opacity` and the rounded edge antialias. The shader's push constants grew an `opacity` field for
that (112-byte block, `tools/shaders/manifest.json`). A renderer without the pipelines, a frame
target that cannot be read, or a pool that cannot hand out both targets paints the node without the
effect and counts it in `GlassPaintStats::unsupported`; nothing throws or blocks. A glass frame in
the window renders into the per-slot offscreen target even at content scale 1, because the copy
source must not be a swapchain image; the same submission resamples it into the acquired image.
`tests/gpu/batched2d/glass_paint_gpu_tests.cpp` proves the pixels through the real paint path and
`glass_paint_null_tests.cpp` the recorded structure and every fallback.

### 5.2 Isolated layers (#227)

**Landing plan (issue #617 `mask`, then `filter`, are the first consumers).** One slice each, every
slice behavior + tests + docs, nothing user-visible until the last:

1. **Sink seam (landed, issue #227 slice 1).** `LayerDraw`, `PaintSink::supports_layers()`/`push_layer`/`pop_layer` (all default off), forwarded by `TextShadowSink` and `LcdTextSink`; `ClippedPaintSink` answers false until slice 2 replays a layer per damage region; `PaintCache::Recorder` records a push op and a `pushed_layer` flag (per node) or a pop op (subtree list), and the pass pops a replayed record's layer after the clip pop. Test: `paint_layer`. Original scope: `PaintSink::push_layer(const LayerDraw&)` / `pop_layer()`, default no-ops, so a sink that
   ignores them paints the subtree flat (today's non-isolated behaviour). `LayerDraw` carries the
   device-space box, the folded opacity and an optional mask. Wired through `ClippedPaintSink`,
   `LcdTextSink`, `TextShadowSink`, the recorder (`PaintCache` ops) and replay, with the structure tests
   that already guard `push_clip`/`pop_clip` balance.
2. **Renderer layer path (landed, issue #227 slice 2).** `Batched2DRenderer::push_layer(box, opacity)` / `pop_layer()`
   (`src/rendering/gpu/batched2d_layer.cpp`). The layer target is a pooled target the size of the frame,
   so it shares the frame's coordinates, transform, clips and scissors and no origin shifts: flush,
   close the frame pass, open a pass on the target cleared to transparent, draw, close, reopen the
   frame pass and composite once per scissor (the frame clip, the layer's device box and each damage
   region). The composite is `glass_blur.frag` mode 2 (and its WGSL twin): the batched pipeline's
   straight-alpha blend over a transparent clear leaves premultiplied colour with a correct alpha, so
   mode 2 divides the colour back and applies the group opacity once; the composite pipeline and
   pool are the glass ones, so no new pipeline exists. A layer that cannot be isolated (no composite
   pipeline or pool, a multisampled frame, an empty or off-frame box, or the bounded pool out of
   targets: the default pool of 4 holds four layers live at once, section 5e) paints flat:
   its opacity multiplies into every primitive until the pop, which is the §14.1 rule. A
   `backdrop-filter` inside an isolated layer sees the layer's content only, which is the CSS
   backdrop-root behaviour. Tests: `layer_null` (pass and draw skeleton, nesting, the bound, the flat
   fallbacks), `layer_gpu` (overlap isolation against the flat contrast, premultiplied accumulation,
   nesting, scissor, repeated frames, validation clean). Limits to lift later: a frame-sized target
   per layer (a box-sized target needs an origin shift of the transform and the clip records) and
   the pool bound. Original scope: `Batched2DRenderer::push_layer`: flush, `break_pass()`, acquire a pooled target
   for the box, draw the subtree into it (render pass on the target, clip and transform rebased to the
   box origin), `pop_layer` composites it once through `bind_sampled_target`, then `resume_pass()`. A
   pool that cannot hand out a target, or a frame target that cannot be read, paints flat and counts it
   in a `LayerPaintStats::unsupported`, like the glass fallback. Null-backend structure test first, then
   the lavapipe pixel test.
3. **`CpuCanvas` twin (landed, issue #617).** `CpuPaintSink` reports layers and masks, so the CPU path and the goldens match the GPU path. The canvas is an opaque framebuffer, so a layer cannot accumulate premultiplied alpha in it; instead every draw inside a layer goes to two frame-sized scratch canvases, one cleared opaque black and one opaque white, and the pop recovers per pixel `P = black` and `A = 1 - (white - black)` (the rounded mean of the three channel differences) and writes `backdrop * (1 - opacity * A) + P * opacity` once, inside the layer box and the active clip (`composite_layer`, `include/tessera/rendering/software/cpu_layer.hpp`). The composite is linear in the backdrop, so a nested layer composites into the outer pair and both canvases stay consistent. A mask image is drawn into its own transparent pair, so seams and overlaps are the ordinary blend's, and `apply_layer_mask` scales the layer pair by its alpha (the GPU's DestinationIn step); a layer takes one mask and a sink with no open layer refuses, like the renderer. No replay of recorded ops was needed. The same change makes the CPU linear gradient use `band_edge` like the GPU sink (it still grew each band by half a pixel, so a translucent ramp composited twice there); only `tests/golden/baseline/cpu_wide/layers.png` moved, whose top ramp is translucent. Tests: `cpu_layer` (pair math, composite, nesting, mask, on exact bytes), `cpu_layer_sink` (sink calls, refusals, group opacity through the pass) and the `masks` scene of `cpu_wide_golden`. **Lavapipe golden:** `layer_mask_golden` (`tests/gpu/batched2d/layer_mask_golden_tests.cpp`, baseline `tests/golden/gpu/mask_image.png`, regenerated with `--update-goldens` on a software device only) paints that same scene through the paint pass and the renderer's mask step, refuses a hardware device, requires two byte-identical frames, compares bit-exactly with the baseline and compares with the CPU twin; the measured worst channel difference between the two paths on llvmpipe is 1 of 255 (the test allows 2). The scene has four masked nodes and the test still raises `max_offscreen_targets` to 8 from the days when the pool of 4 masked only the first two nodes of a frame; sibling layers now recycle their targets (section 5e), so the default pool masks all four like the CPU twin. Original scope: The same layer on the CPU canvas, so goldens and `CpuPaintSink` agree with the
   GPU path to the existing tolerance.
4. **Group opacity becomes isolated (landed behind `PaintContext::isolate_group_opacity`, default false, issue #227 slice 4).** `paint_node` opens a layer after the node's transform push and before its backdrop, shadow and box paint, closes it after the children, the clip pop and before the transform pop; the node's own opacity moves to the layer and the primitives inside carry only the ancestor product; text inside a layer is grayscale (`PaintScope::in_layer`); `isolate_groups` and `in_layer` are part of the paint-cache key. The box is the whole frame for now (the renderer clamps it to the frame, the clip and the damage). A culled node still fades its visible children flat. Tests: `paint_layer_pass` and the paint-path case of `layer_gpu`. Original scope: **Group opacity becomes isolated** only where it is observable (overlapping children), behind the
   same seam; the §14.1 product rule stays the fallback.
5a. **Renderer mask path (landed, issue #617 slice 2; RHI 1.19).** `Batched2DRenderer::begin_layer_mask()` /
   `end_layer_mask()` (`src/rendering/gpu/batched2d_layer_mask.cpp`), called after a layer's content and
   before its `pop_layer()`. The mask image is drawn as ordinary content into a second pooled target
   cleared to transparent, so seams, anti-aliasing and overlaps are the normal blend's (a mask drawn
   straight into the layer with a destination-in blend would multiply at every AA edge and seam). At
   `end_layer_mask()` that target is sampled into the layer target by the composite program
   (`glass_blur.frag` mode 2, opacity 1) through a pipeline with `BlendMode::DestinationIn`
   (`GlassPipelines::mask`), which scales every channel of the premultiplied layer by the mask's alpha:
   `mask-mode: alpha`. No new shader, descriptor or capability. A mask that cannot run (no mask
   pipeline, a flat or already-masked layer, no second pooled target) returns false and records
   nothing, so the caller skips the mask draws and the group paints unmasked, counted in
   `layer_masks_unsupported()`. Cost: one more pooled target and one fullscreen draw per masked layer
   (two live targets while a mask is open; sibling masked layers recycle them, section 5e). Tests: `layer_mask_null` (skeleton and every
   fallback) and `layer_mask_gpu` (opaque, half and undrawn mask, composition with opacity, the
   unmasked fallback, repeated frames, validation clean). 
5b. **`mask-image` in the paint pass (landed, issue #617 slice 3).** `PaintSink::begin_layer_mask()` /
   `end_layer_mask()` (default: refuse), forwarded by the wrapper sinks and implemented by
   `Batched2DSink`. `paint_node` opens a layer for a node whose `mask-image` parses to at least one
   gradient (`has_mask_image`), whatever `isolate_group_opacity` says; the node's own opacity moves to
   the layer. After the children and the clip pop, and before `pop_layer`, it asks the sink to mask
   and, if the sink agrees, emits the mask image (`paint_mask_image`: the same layer list and
   gradient draws as `background-image`, border box, unrounded, no opacity fold). A sink that cannot
   layer paints the node unmasked; one that refuses the mask paints the layer without the mask draws.
   **Cache:** the mask paints after the children, which a per-node `PaintRecord` (prefix plus pop
   flags) cannot hold, so a masked node is never per-node recorded and its stale entry is erased when
   the mask appears (`paint_mask` pins that). Inside a retained subtree draw list the mask is recorded
   as `BeginLayerMask`, the gradient fills and `EndLayerMask`; the recorder always records the fills
   and forwards them only while its target agreed to mask, and replay skips a mask the replaying sink
   refuses. Tests: `paint_mask` (stream order, opacity, every degraded case, stale-record guard,
   subtree record and replay) and the paint-path case of `layer_mask_gpu`. **Chromium comparison (measured 2026-11-27).** The same
   scene in Chromium and through the paint path (a 64x64 black frame, a #00ff00 box at (8,8) 48x48,
   linear, radial and conic masks, opacity 1 and 0.5; reference values in
   `tests/gpu/batched2d/layer_mask_chromium_reference.hpp`, asserted by `layer_mask_gpu`). The mask
   path matched at once: nothing outside the box, only green inside, the node opacity composes with
   the mask exactly, a linear ramp exact across its axis. **It also found a bug, now fixed:** alpha
   along a translucent gradient came out up to +64 of 255 too high for linear, +72 for conic and +119
   on the radial diagonal, never meaningfully below Chromium. Cause: the sink drew a gradient as
   solid bands each grown by half a pixel (`band + 0.5`), so neighbouring bands overlapped and a
   translucent stop composited twice there; opaque gradients hid it, and any `background-image` with
   a translucent stop had the same error. **Fix:** the linear bands and the radial and conic cells now
   abut exactly (`band_edge` in `paint_sink.hpp`: shared edge values, the far end exact). A plain
   quad is hard-edged (the pixel-centre rule on the GPU and in `CpuCanvas`), so each pixel belongs to
   exactly one band and no seam protection is needed. After the fix the linear ramp is within 2 of
   Chromium, the radial cells within about 8 (the 32x32 cell quantisation, paint-pass.md section 13)
   and the conic cells within 5. The conic grid was raised from 32x32 to 64x64 (`kConicBands`) after
   the comparison showed +/-19 at 32x32. `tests/golden/baseline/cpu_wide/radials.png` (294 of 64000
   pixels) and `conics.png` changed; the opaque linear, clip, layer and shadow baselines are
   byte-identical. Regression tests:
   `paint_gradient` (cells tile a fractional box with no gap and no overlap), `layer_mask_gpu` (the
   Chromium comparison, and a translucent `background-image` ramp at a fractional position with
   anti-aliasing on and off, within 6 of the ideal ramp), `cpu_wide_golden`; reintroducing the
   overlap fails all three. (Superseded: gradients are now one per-fragment quad, [gradients.md](gradients.md); the overlap, bands and cell errors above no longer occur on the shader path, which measures 0-1/255 against Chromium.) Still open: the lavapipe golden.
5c. **Mask geometry and the `mask` shorthand (landed, issue #617).** `mask-size`, `mask-position`,
   `mask-repeat` and `mask-mode` (`include/tessera/ui/style/mask_layers.hpp`) are per-layer lists that
   repeat cyclically over `mask-image`. `paint_mask_image` reads them and, per layer, asks
   `compute_mask_tiles` (`src/ui/paint/paint_mask_tiles.cpp`, pure arithmetic) for the tile rects: size
   resolves against the border box (a gradient has no intrinsic size, so `auto`/`cover`/`contain` are
   the box), position is `(area - tile) * fraction + px`, `repeat` backs up to the first tile reaching
   the box, `space` spreads whole tiles with an even gap, `round` rescales to a whole count. Each tile
   is one ordinary gradient draw resolved against the tile; if any tile reaches outside the box the
   layer's draws sit in a `push_clip`/`pop_clip` of the box, so the mask still covers the border box
   only. A layer whose geometry is the initial one takes the old single-draw path unchanged. More than
   256 tiles (each tile is up to 1024 quads) or a zero-size tile paints nothing for that layer.
   `mask-mode: luminance` rewrites every resolved stop's alpha to luminance (sRGB 0.2126/0.7152/0.0722)
   times alpha, so it needs no shader: exact for opaque stops, and for translucent ones the encoded-space
   ramp interpolates luminance-times-alpha rather than luminance of the interpolated colour (a small
   difference, documented not measured). `match-source` is `alpha` for gradients. Not implemented:
   `mask-origin`, `mask-clip`, `mask-composite` (the `mask` shorthand rejects them whole), em/rem/calc
   lengths. The `mask` shorthand (`property_mask.cpp`) expands each layer into the five longhands.
   Tests: `mask_layers` (parsers, lists, registry grammar, shorthand) and `paint_mask_geometry` (tile
   placement, repeat/space/round, clip, layer cycling, cap, luminance; mutation-checked).
5d. **`filter: blur()` / `drop-shadow()` (landed, issue #617 slice 4).** The chain rides in the layer's
   own push: `LayerDraw::filter` (`rendering::gpu::LayerFilterChain`, at most three
   `LayerFilterOp`s of 20 bytes each - the shadow colour is packed 8 bits per channel so `PaintOp` stays
   inside its `paint_op_size` budget - copied by the paint cache like any other op), so the sink seam, the wrapper
   sinks, the recorder and replay needed no new call. The paint pass (`paint_filter.cpp`) parses the
   computed `filter` (`style::parse_filter`: `none` or `blur(<length>)` / `drop-shadow(<color>?
   <length>{2,3})`; adjacent blurs fold by adding variances; a zero blur and an invisible unblurred
   shadow are dropped; any other function drops the whole declaration with one `UnsupportedValue`),
   clamps each sigma to `glassMaxBlurRadius`, resolves `currentcolor`, and opens a layer for the
   node like a mask does, whatever `isolate_group_opacity` says. Like a masked node it is never
   per-node recorded (its layer state would otherwise have to survive a sink change); the
   subtree draw list records the push with the chain. The drop-shadow blur is a standard deviation
   (CSS Filter Effects), not the `box-shadow` blur radius, which is twice a sigma.
   **Renderer** (`batched2d_layer_filter.cpp`, `glass_filter.cpp`): the chain runs once, when the layer
   is finished - at `begin_layer_mask()` or `pop_layer()`, whichever comes first - so it sees the
   complete content and precedes the mask and the opacity, as CSS orders filter, mask, opacity. It
   reuses the glass blur program (`glass_blur.frag`), so no shader, descriptor or capability is
   added:
   - *blur*: horizontal pass layer -> scratch (mode 0, no blending), vertical pass scratch -> layer
     (mode 0, into a transparent clear). The layer holds premultiplied colour with a correct alpha,
     so the linear-filtered Gaussian over all four channels is the premultiplied blur CSS specifies.
   - *drop-shadow*: the same horizontal pass, then the vertical pass runs through the mask
     (`DestinationIn`) pipeline in mode 1 over a third target that was cleared to the premultiplied
     shadow colour. Mode 1 samples at `fragment - region_origin`, which is the offset, and a box large
     enough that its rounded-box coverage is 1 everywhere; the blend multiplies the colour by the
     blurred, offset alpha. The layer is then composited over that target (the ordinary straight-alpha
     source-over onto a premultiplied destination) and the target becomes the layer.
   Offsets follow the active transform's linear part, sigmas scale by the mean content scale. The
   passes are limited to the layer's scissors (frame clip, damage regions) and the horizontal pass to
   their union grown by the reach. Pooled targets: a blur takes one scratch target, a shadow one more
   (a spare is reused by the next shadow of the chain), so a filtered layer holds two or three of the
   default four and a filtered, masked layer three or four. A function that cannot run (no blur
   pipeline, no mask pipeline for a shadow, no free target) is skipped and counted in
   `layer_filters_unsupported()`; a flat layer drops its whole chain. A filtered layer whose passes
   all fall back records exactly the plain layer stream, and one that ran leaves the layer pass
   closed instead of reopening an empty one.
   **Damage.** A filter reads pixels up to its reach away, like a backdrop blur reads 3 sigma, but it
   is not glass, so it has its own state (an earlier cut stored reach / 3 as the backdrop sigma, which
   made every filtered tree a glass tree). `sync_scene_style` stores the chain's largest one-sided
   halo in `Node::filter_reach` (16-bit fixed point, rounded up, in the four bytes the backdrop sigma
   shares, so `sizeof(Node)` is unchanged) and the derived `Node::filter_subtree` bit, counts
   `SceneStyleSyncResult::filter_nodes` apart from `glass_nodes`, and adds the halo to the node's own
   shadow reach in the paint outsets (the layer holds the node's shadows and the chain blurs them, so
   the reaches sum rather than max). `DamageTracker::add_filter_dependencies` runs before the glass
   walk, pruned by `filter_subtree`: a filtered node whose region (`node_filter_region`, its cached
   subtree paint box mapped to the world, which carries the halo around the box and around every
   overflowing child) intersects the frame's damage gets the whole region added, because a filtered
   pixel anywhere in it may depend on the changed one; `FrameRender::filter_dependencies` counts them.
   The paint cull keeps a node whose halo is on screen (the outsets carry it) and the layer is drawn
   complete inside the damage regions before it is blurred.
   **Frame target.** `glass_subtree` is the only thing the window reads when it decides to render
   through the per-slot offscreen frame target and resample it into the swapchain image (a glass frame
   copies a region of the frame, and a swapchain image is neither copy source nor samplable). A filter
   runs on pooled layer targets and only composites into the frame, so a filter-only tree keeps the
   direct path: no frame-sized target and no full-frame resample. A node with both a backdrop-filter
   and a `filter` is a glass tree through the glass flag alone. Measured on the null backend
   (`filter_frame_target`, 1920x1080, one `blur(8px)` card): 5 render passes, 5 draws and 3 sampled
   binds either way; routed like glass (the old behaviour) the same frame also holds a second frame
   target (7.9 MB per frame slot) and records one full-frame resample (7.9 MB read + 7.9 MB written per
   presented frame, 15.8 MB). Direct: 1 frame target, 0 blits, 0 region copies.
   Tests: `filter_value` (grammar, registry diagnostic, `@supports`), `paint_filter` (stream, clamp,
   colours, opacity, cache, replay, reach), `layer_filter_null` (pass and draw skeleton of a blur, a
   shadow and a masked blur, every fallback, the bounded pool over many frames) and
   `filter_damage` (own field and flag, no leak between glass and filter, halo plus shadow, the region
   around an overflowing child, the filter walk, cull, frame pipeline), `filter_frame_target` (the window
   sees glass only; a filter frame copies and resamples nothing of the frame target; before/after
   counters), `filter_damage_gpu` (lavapipe: after a child moves inside a blurred node, the
   damage-limited redraw equals a full redraw pixel for pixel; without the filter walk 308 pixels of a
   128x128 frame keep stale blur) and `layer_filter_gpu` (blur against the analytic Gaussian along rows, columns and the corner; a hard
   and a soft shadow under the opaque box; opacity after the filter; the half-plane mask cutting a
   blurred layer exactly at its edge; the paint path for blur, shadow and a two-function chain; the
   fallbacks; recycled targets; validation clean). **Chromium comparison (measured 2026-11-27).** A
   64x64 black canvas with a #ff0000 box at (24,24) 16x16 drawn through `ctx.filter` (the CSS filter
   pipeline): `blur(2px)` along row 32 reads 0, 10, 57, 153, 229, 255, 198, 57, 3, 0 at x = 16, 20,
   22, 24, 26, 31, 38, 41, 44, 48 and 13 at the (22,22) corner; `drop-shadow(8px 6px 2px
   rgba(0,0,255,.5))` reads 98, 99, 95, 77, 10 (blue) at x = 36, 42, 44, 46, 50 on row 44, and a
   zero-blur shadow reads 128. The renderer matched at once (within 4 of 255, and equal to the
   analytic Gaussian with sigma = the written length, so the third `drop-shadow()` length is a
   standard deviation); the values are asserted by `layer_filter_gpu`. Still open: the other `filter` functions (`brightness`, `contrast`, `opacity`, ...), a
   `CpuCanvas` twin and the lavapipe golden.
5. **`mask-image`** (gradient images first: the engine already parses all three gradient shapes) as the
   layer's alpha multiplier, then `mask-mode`/`mask-size`/`mask-position`/`mask-repeat`, then `filter`
   (`drop-shadow()`, `blur()`, landed in 5d) as a layer post-process. Each with a Chromium comparison and a lavapipe
   golden, and the property-status table updated.
5e. **Pool sizing, recycling and observability (landed, issue #617).** The pool used to hand a
    target out once per frame and take it back only after `frames_in_flight` frames, so the default
    pool of four held two masked layers (layer + mask) and every later masked or filtered node of the
    frame silently painted without the effect; and because the window rings `frames_in_flight =
    kFramesInFlightPerWindow * kMaxWindowsPerDevice = 8`, a layer used on every frame found its targets
    still "in flight" after four frames and painted flat until the ring wrapped. Three changes, each
    tested on the null backend (`layer_pool_null`, `offscreen_pool`) and on a device
    (`layer_pool_gpu`):
    - **Recycle when dead.** `GlassCompositor::release` / `OffscreenTargetPool::release`. The renderer
      releases a mask target right after its `DestinationIn` multiply, the filter scratch (and the
      shadow's spare) after the chain, a layer target right after its composite, and a glass node's
      source and blur targets after its composite. The next acquire, in the same frame or the next,
      takes the target again. The bound that matters is the **peak live targets**:

      | scene | peak live | why |
      |---|---|---|
      | any number of plain sibling layers | 1 | the layer |
      | sibling masked layers | 2 | layer + mask (mask dead after the multiply) |
      | sibling blurred layers, blurred then masked | 2 | layer + scratch; the mask takes the scratch |
      | sibling drop-shadow layers (masked or not) | 3 | layer + scratch + shadow target |
      | glass backdrops, any number of nodes | 2 | source + blur, per node |
      | nested layers, depth d, innermost masked | d + 1 | one per open layer + the mask |
      | nested, innermost drop-shadow | d + 2 | one per open layer + scratch + spare |

      The default count cap of 4 therefore covers a masked layer nested three deep or a shadow nested
      two deep; deeper nesting falls back as before, but now counted (next bullet).
    - **Backend ordering.** Reusing a target inside a command list needs the new pass ordered after the
      old reads. Vulkan `begin_render_pass` now uses the target's tracked layout as the barrier's
      source scope for a cleared pass on a target the list already used (it used TOP_OF_PIPE, which
      orders nothing after a fragment-shader read: a write-after-read hazard), and a bind of a sampled
      target is consumed when the pass that sampled it ends (`sampled_target_` was only cleared by
      `bind_texture`, so the first reuse of the target just composited threw). WebGPU already
      cleared its sampled binding at the end of the pass; the queue executes submissions in order, so
      the barrier's first scope reaches back across frames. A target is still destroyed only after
      `frames_in_flight` frames.
    - **Hard memory cap.** `max_offscreen_bytes` (256 MiB) next to the count cap. The RHI reports no
      heap budget or device limit, so the cap is a fixed default sized from the frame: a target is
      width x height x 4, 8.3 MB at 1080p, 33.2 MB at 4K, 132.7 MB at 8K, so 256 MiB lets the count cap
      (4) bind up to 4K (4 x 33.2 = 133 MB) and holds one 8K target. Retired targets (a resize) count
      against it.
    - **Idle release.** A free target unused for `offscreen_idle_trim_frames` (120) frames is destroyed
      at the next `begin()`; a driver with nothing to draw calls `trim_offscreen_targets(true)` after
      its drain, so a static UI holds zero layer memory (the pool's slots stay reserved, the images
      go). Steady state allocates nothing: the pool reserves its slots at construction, and the
      `offscreen_pool` test counts `operator new` across 400 frames of acquire/release.
    **Observable fallback.** When a cap still refuses a request the behaviour is unchanged - the layer,
    mask, filter or glass node paints without the effect - but it is counted: `offscreen_stats()`
    reports `exhausted` (this frame) and `exhausted_total` next to `peak_live`, `peak_live_max`, `bytes`
    and `peak_bytes`, and `layers_flat()` / `layer_masks_unsupported()` / `layer_filters_unsupported()`
    keep saying which effect was lost. The window driver samples the pool after every frame
    (`window_offscreen.cpp`): `WindowFrameStats::offscreen_peak_live`, `offscreen_bytes` and
    `offscreen_refused`, and a `diagnostics::OffscreenPressureReporter`
    (`include/tessera/diagnostics/offscreen_pressure.hpp`) that adds the refused requests to the
    `gpu.offscreen.refused` counter, sets the `gpu.offscreen.peak_live` and `gpu.offscreen.bytes`
    gauges, and writes ONE `TESSERA_GPU_OFFSCREEN_EXHAUSTED` warning per run (category `gpu`, with the
    peak, the targets, the bytes, and the knob to turn) through the `Logger`. `WindowOptions::logger` and
    `metrics` supply them; with no logger the window logs to its own stderr `FileLogSink`, with no
    registry the metrics are skipped. A single-window driver that has used no layer for
    `WindowOptions::offscreen_idle_seconds` (default 2), with its in-flight frames complete, calls
    `trim_offscreen_targets(true)` from its settled-scene branch (`run()` and `pump_once()`),
    so a static UI holds no layer memory; a multi-window group relies on the 120-frame trim, because
    another window's frames may still read the shared pool. Tests: `diagnostics_offscreen` (counter,
    gauges, one warning, optional halves, the stderr line format) and `app_window_offscreen` (a real
    window: six masked boxes peak at two targets and refuse nothing; a pool capped at one target
    counts the refusals, reports them in the stats and logs exactly once; an idle window with a
    0.15 s threshold destroys its targets and a later restyle rebuilds them with nothing refused;
    mutation-checked by removing the trim call).
    **Measured (1920x1080, 12 masked sibling cards, default config, null backend sizes; a target is
    exactly 8,294,400 bytes).**

    | | before | after |
    |---|---|---|
    | layers isolated / flat | 2 / 10 | 12 / 0 |
    | masks applied / unsupported | 2 / 10 | 12 / 0 |
    | peak live targets | 4 (cap) | 2 |
    | pooled targets / GPU bytes held | 4 / 33,177,600 | 2 / 16,588,800 |
    | pooled targets after 120 idle frames | 4 | 0 |

    On the GPU the test scene (6 and 12 masked cards plus three blurred, masked cards, ring depths 1, 2
    and 8, three frames each) matches the expected pixels and is byte-identical frame to frame, with no
    validation message; the CPU twin, which has no pool, masked all of them all along.

**Integration points found in `paint_node` (`src/ui/paint/paint_pass.cpp`), to settle in slice 1:**

- *Where the layer opens.* After the node's own `push_transform` and before its backdrop, shadow and
  box paint, so the node's box and its children are one group; it closes after the children and
  before `pop_transform`. The existing `push_clip` wraps only the children and stays inside the layer.
- *Layer box and space.* The device-space axis-aligned bounds of the node's visual box (box shadow
  included) under the active transform, clamped to the clip and the frame. A rotated node composites
  its bounding box, not its rotated rectangle; the composite draws the layer's own quad through the
  same transform, so only the target allocation is the AABB.
- *Opacity.* Today every primitive multiplies the ancestor opacity product (`child_scope.opacity`,
  section 14.1 of `paint-pass.md`). A layer applies the node's own opacity once, at composite, so
  inside a layer the child scope restarts at 1.0 and the record's `effective_opacity` is the
  product outside it. A sink without `supports_layers()` keeps today's product rule, so the two
  paths must agree on non-overlapping content (golden) and differ only where children overlap.
- *Cache.* A per-node `PaintRecord` holds the node's own ops and pop flags. A layer adds
  `pushed_layer` and the `LayerDraw` to the record (the pass pops it after the child walk, like
  `pushed_clip`), and the subtree draw list (`record_pops`) records `PushLayer`/`PopLayer`
  explicitly. A layer node's descendants' damage must dirty the layer's whole box, like a glass
  node's dependencies (`DamageTracker::add_glass_dependencies`).
- *Producers stay off until slice 2 is proven.* `supports_layers()` is false on every sink at first,
  so slice 1 changes no recorded stream and no golden.

Open decisions for the owner before slice 2: the composite needs a new shader (GLSL, WGSL and MSL
twins through the shader chain, so it touches the `shader-cross` and `wgsl-validation` lanes), and
the layer target's format and colour space must follow the sRGB/UNORM target rule.

An isolated layer is the same seam with a different destination: render the subtree into a pooled
target (render pass on the target, not a region copy), then composite it once. Because the pool
hands out plain `RenderTarget`s - not region-copy scratch buffers - no new resource type is needed;
the layer path only needs a composite draw with the target as a texture, which is the same
`bind_sampled_target` the glass composite uses ([`rhi.md`](rhi.md) section 5.1).
#227 itself shipped the non-isolated rule instead - the effective opacity product folded into
every primitive of the subtree, no offscreen target ([`../paint-pass.md`](../paint-pass.md)
section 14.1) - so true group isolation, shared-element snapshots and stacking contexts still
wait on this seam.

## 6. Testing and determinism

- `tests/gpu/batched2d/renderer_seam_tests.cpp` (null, always runs): break/copy/resume records two
  passes, one copy record with the exact region and offset, and the same draws; an unbroken frame
  records exactly the old single-pass stream; invalid copies (in-pass, out-of-range, multisampled,
  format mismatch, same target, empty region) throw; the pool stays bounded over 300 frames and
  honours `frames_in_flight`.
- `tests/gpu/batched2d/renderer_seam_gpu_tests.cpp` (lavapipe/real Vulkan, skips without a device):
  draw, break, copy, resume, draw -> expected pixels; content before the break survives the Load
  resume; the copy holds the source pixels; a copied region can be placed back into the frame
  target.
- `tests/gpu/batched2d/sampled_target_tests.cpp` (null, always runs): the sampled seam records
  attachment -> copy -> sample -> composite in that order (`NullCommandStats::command_order`), the
  target's sampler state reaches the backend from `RenderTargetDesc`, a bind inside a pass and a bind
  of a target without content are rejected, the renderer path breaks, copies into a pooled target,
  samples it into a second pooled target and composites, and a frame that samples nothing records no
  sampled bind.
- `tests/gpu/batched2d/glass_composite_gpu_tests.cpp` (lavapipe/real Vulkan, skips without a device):
  the first real glass pixels. It draws a red/blue backdrop, breaks, copies it, runs the horizontal
  `glass_blur` pass into a pooled target sampling the copy, resumes and composites through the
  vertical pass. Expected pixels come from `glass_kernel_taps` plus a bilinear clamp-to-edge sample,
  so the shader and the CPU kernel cannot drift; the same test reads both sampled targets back
  afterwards, which exercises the shader-read -> transfer transition in the other direction.
- `tests/gpu/batched2d/glass_paint_null_tests.cpp` (null, always runs) drives the REAL paint path
  (`paint_scene` -> `Batched2DSink` -> `Batched2DRenderer::draw_backdrop`): one glass node records
  break -> copy -> horizontal pass -> resume -> composite with two pooled targets, two sampled binds
  and the exact pass count; a renderer without glass pipelines, a pool bounded to one target and
  `blur(0)` each paint the node without the effect, stay balanced and are counted
  (`GlassPaintStats::unsupported`); a tree without a glass node records one pass, no copy and no
  sampled bind.
- `tests/gpu/batched2d/glass_paint_gpu_tests.cpp` (lavapipe/real Vulkan, skips without a device)
  drives the same real paint path and checks the pixels: a uniform backdrop composites to the
  analytic colour, a red|blue split under `blur(3px)` is compared column by column against the CPU
  kernel reference derived from the renderer's own region plan, the half-resolution rule still blurs
  the same edge, the rounded clip leaves the backdrop outside the corner radius, the opacity fold and
  `blur(0)` pass-through hold, and the run is validation-clean.
- `tests/gpu/batched2d/offscreen_pool_tests.cpp` (null, always runs): the pool alone - release and
  reuse in a frame and at ring depth 8, the never-released rotation rule, retirement of an in-flight
  target on growth, both caps, idle trim and explicit trim, the counters, and zero allocations over
  400 steady-state frames.
- `tests/gpu/batched2d/layer_pool_null_tests.cpp` (null, always runs): 1, 2, 6 and 12 sibling masked
  layers all isolate and mask on two targets, the peak-live table of section 5e, glass cards, nested
  layers up to and past the cap (counted, graceful), the byte cap, idle trim, and 300 frames at ring
  depths 1, 2 and 8. `layer_hazard_device.hpp` wraps the null device and fails a stream that clears a
  target whose last write nobody sampled (an early release) or passes on a still-bound sampled target;
  a test shows it catching one.
- `tests/gpu/batched2d/layer_pool_gpu_tests.cpp` (lavapipe/real Vulkan, skips without a device): the
  same scene in pixels, validation clean.
- Existing image goldens are unchanged: no golden scene breaks a pass or samples a target, so the
  single-pass command stream and its pixels are untouched, and a no-glass frame records no sampled
  bind.

## 7. Deferred / open

- Same-target (in-image) region copies, and a swapchain image as a copy source/destination (needs
  swapchain usage flags; see [`rhi.md`](rhi.md) section 10). The same applies to sampling: a
  swapchain image is not a sampled target in RHI 1.3 because it is created for color
  attachment/transfer-dst, and making one samplable is a swapchain-usage decision.
- Multiple sampler states per target (the descriptor carries one), mip generation for a sampled
  target, and sampling a target while it is the pass's own attachment - all deliberately out of the
  capability's scope.
- The #227 layer path: render a subtree into a pooled target and composite it once. The glass
  composite (#244) landed its half of this consumer in
  [`src/rendering/gpu/glass_compositor.cpp`](../../../src/rendering/gpu/glass_compositor.cpp) -
  `break_pass`/`copy_target_region`/`bind_sampled_target`/`resume_pass` plus the two pipelines - so a
  layer is that sequence with a render pass instead of a region copy. Everything it needs is in the
  capability, not RHI work.
- WebGPU is implemented but compiled only in the Wasm lane (no Emscripten on every dev machine);
  `copyTextureToTexture` and the sampled bind group are validated by that lane, not locally on
  Windows.
