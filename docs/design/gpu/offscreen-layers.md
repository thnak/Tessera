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

The pool (`include/tessera/rendering/gpu/offscreen_target_pool.hpp`) is bounded and frame-aware:

- **Bound:** `Batched2DConfig::max_offscreen_targets`, which is `glassMaxBlurTargets` from
  [`glass-v1.md`](../../specs/glass-v1.md) section 5, default **4**; 0 disables the pool
  (`acquire_offscreen_target` returns null and the caller paints without the effect).
- **Retirement:** a target acquired in frame F is referenced by F's command list, so it is handed
  out again only after `frames_in_flight` frame slots have rotated - the same "frame F is complete"
  point the vertex-buffer slots already rely on. A target is never destroyed while the GPU may still
  read it, and the pool never grows past the cap: when every target is in flight, `acquire` returns
  null and the caller skips the effect for that frame (the bounded-cost rule of
  [`performance.md`](../performance.md) section 6, not a quality fallback).
- **Reuse:** a steady-state screen acquires one target per frame and reuses it for the whole run
  (the 300-frame null test keeps `offscreen_target_count()` at one with `frames_in_flight = 1`, at
  two with `frames_in_flight = 2`). Consecutive acquires in one frame return different targets, so
  a two-pass blur can ping-pong.
- **Sizing:** a free target grows to the largest extent requested on it and keeps that high-water
  extent; a target used by the current frame is never recreated mid-frame, because a recorded copy
  may still reference its image. Targets use the frame target's format and a transparent-black
  clear.
- **Allocation:** targets are created on demand at a frame boundary and reused forever after; no
  per-draw or per-glass-node allocation happens on the hot path, and nothing blocks the UI thread.

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

The renderer wiring landed with #244 in `src/rendering/gpu/batched2d_glass.cpp`
(`Batched2DRenderer::draw_backdrop`), driven by `Batched2DSink::draw_backdrop` on the real paint path:

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
  [`src/rendering/gpu/batched2d_glass.cpp`](../../../src/rendering/gpu/batched2d_glass.cpp) -
  `break_pass`/`copy_target_region`/`bind_sampled_target`/`resume_pass` plus the two pipelines - so a
  layer is that sequence with a render pass instead of a region copy. Everything it needs is in the
  capability, not RHI work.
- WebGPU is implemented but compiled only in the Wasm lane (no Emscripten on every dev machine);
  `copyTextureToTexture` and the sampled bind group are validated by that lane, not locally on
  Windows.
