# Retained layers: keeping the pixels of a static masked, filtered or faded group

Status: implemented for issue #617 on top of the isolated layers of
[`offscreen-layers.md`](offscreen-layers.md) section 5.2. The seam, the pool and the pass it builds
on are described there; this note is the design of the cache: what it keeps, when an entry is stale,
what it costs and how it gives memory back.

## 1. Problem

A node with `mask-image`, `filter` or (with `isolate_group_opacity`) a group `opacity` paints as an
isolated layer: its subtree is drawn into an offscreen target, then the filter, the mask and the
opacity run, then the result is composited. That is several render passes, a fullscreen draw per
step and a walk of the subtree, **every time the node is painted**, whether or not anything inside it
changed. A masked card that has not moved for a minute still costs the same as one that is animating,
on every frame that repaints its area: a full redraw (resize, theme or scale change, an animated
backdrop), or a damage region that merely overlaps it.

The cache keeps the finished pixels of such a node while it is unchanged and composites them
instead. The subtree is not painted, the mask and the filter do not run, and the layer's render
passes are not recorded.

## 2. What is kept

One entry per owner node: the layer after its filter and mask and **before** its group opacity, as
premultiplied colour, in an exact-size render target holding the device-pixel region the layer
covered (the node's subtree paint box, grown by the filter's reach, clamped to the frame and the
clip). Opacity stays out of the entry so the composite applies it, but a change of opacity still
dirties the node, so today it re-records (see section 10).

- **Record** (`Batched2DRenderer::push_layer`/`pop_layer`, `batched2d_layer_retain.cpp`). On a miss
  for a clean node the layer renders its **whole** region, not just this frame's damage, because the
  pixels must serve any damage a later frame brings: the renderer switches the frame's damage off
  while the layer renders and restores it before the composite. When the layer is finished (after
  its filter and mask) its region is copied 1:1 into the reserved target
  (`copy_target_region`; RHI 1.1, nothing new), and the frame's own composite proceeds as before.
- **Hit.** The layer opens no pass. At `pop_layer` the retained target is sampled by the same
  composite as a live layer (`glass_blur.frag` mode 2) with `region_origin` set to where the entry
  sits in the frame, once per scissor (the frame clip, the entry's region and each damage region).
  The shader and the pipelines are unchanged; mode 2 already mapped `(fragcoord - region_origin) /
  region_size`, and a live layer simply had a zero origin.

## 3. The question the paint pass asks

`PaintSink::retain_layer(LayerRetention{owner, key, clean})` is called right before `push_layer` for a
node that may keep its pixels, when `PaintContext::retain_layers` is on and the per-node cache is in
use. The answer is the whole protocol:

| Answer | Meaning | What the pass does |
|---|---|---|
| true | the sink holds the pixels | pushes the layer, pops it at once, paints no box, children or mask (`PaintStats::retained_layers`) |
| false | draw it | paints as before; a *clean* node may be kept at the pop, a *dirty* one drops its entry |

A node that may be retained is never per-node recorded (like a masked or filtered node already was):
its prefix cannot be replayed without its children. In a **subtree draw list** the question is an op
(`PaintOp::Kind::RetainLayer`) recorded before the layer's push. A replay asks again: on true it
skips every op up to the matching `PopLayer` and runs the pop, which composites. A *recording*
pass over a sink that says true still paints and records the whole layer (a replay later may meet
a sink that has let go), and the recorder forwards none of it
(`PaintCache::Recorder::retained_depth_`). `Batched2DSink` answers through
`Batched2DRenderer::retain_layer`.

**Masks with `mask-composite`.** A retained layer's mask, and the part brackets of its non-`add`
mask layers (`BeginMaskSubtract/Intersect/Exclude` ... `EndMaskLayer`, issue #617), sit inside the
layer's push/pop, so a yes skips them with the layer and the renderer's retained pixels already hold
the composed mask. The recorder keeps the brackets balanced the same way: `mask_live_` is false while
`retained_depth_ > 0`, so `part_live_` is false, the recorded part ops are not forwarded and
`end_mask_layer` forwards nothing; a replay skips the whole span. A sink that declines a part
(`begin_mask_layer` false) skips it to its `EndMaskLayer`. A hit layer takes no mask at all.

## 4. Invalidation

An entry is used only if **every** input of its pixels is the same as when it was rendered. Three
mechanisms cover them; the combination is conservative, and over-invalidating costs a re-render,
never a wrong pixel.

| Input | Covered by |
|---|---|
| node geometry, transform, scroll, painted world matrix, clip stack, inherited opacity, device scale, text-metrics source, `reduce_effects`, `high_contrast`, `lcd_text`, glass limits, list ordinal | the key: `layer_retention_key` hashes the node's whole `PaintCacheSignature` |
| the node's own opacity, its filter chain, whether it is masked | the key |
| frame extent and format, content scale | the renderer mixes them into the key; a different extent or format also drops every entry |
| the node's computed style and every descendant (colours, text, geometry, scroll) | the dirty contract of the paint cache: `clean` is `Dirty::Clean` on the node **and** its subtree. A dirty node is told to the sink, which drops its entry |
| a style or tree generation change whose flags stay Clean | `FramePipeline::invalidate_paint_cache()` bumps the **retain epoch**, which is part of every key |
| a frame that changed nodes without the layer's sink seeing it | the epoch is bumped by a partial repaint (it goes through `ClippedPaintSink`, which cannot layer), by a paint with an interpolation view, and by a frame that had dirty nodes and was not painted in full |
| a dirty node the cull would have skipped | with retention on, a node whose subtree is dirty is never culled, so it is seen and dropped |
| content that was not final when it was drawn (an image still loading, a glass node that could not run, a nested layer that fell back flat, a mask or filter that was unsupported) | the renderer counts these while a layer records; a layer whose count moved is **not kept** (`note_layer_degraded()`) |

What is *not* covered: text glyph-atlas pressure inside a retained layer (a glyph that could not be
placed is not reported by the text renderer yet), so a layer over a saturated atlas could keep a
missing glyph until its node changes or the epoch moves.

## 5. Budget

The entry shares the byte accounting of the live layer targets, so one number still bounds every
byte of layer memory:

- **Bytes.** A retained target is `region.width * region.height * texel`, charged to the pool's
  `max_offscreen_bytes` (default 256 MiB) at creation and refunded at destruction
  (`OffscreenTargetPool::charge/refund/retire`). A second cap, `Batched2DConfig::max_retained_bytes`
  (default 64 MiB, one quarter of the pool's), keeps a share for live layers: with the default pool
  of four live targets, a 4K frame needs 133 MB live and a retained working set fits in the rest.
  The cache never takes the live path's bytes: if the pool is full a record is **refused**
  (`RetainedLayerStats::refused`), the layer paints as usual and nothing else changes. Live
  acquisition is never refused *for* a retained entry.
- **Count.** `max_retained_layers` (default 32, 0 disables retention). It does not use the
  `max_offscreen_targets` slots: a retained target is not a pooled target, it is created exact-size
  when recorded.
- **Eviction.** Least recently used, but **never an entry used this frame**: a working set larger
  than the cap keeps what it has and refuses the rest instead of evicting and re-recording in a
  loop. An entry is also dropped when its owner records a new key (one entry per owner), when its
  node is dirty, after `offscreen_idle_trim_frames` (120) frames of non-use, on a frame-extent or
  format change, and by `trim_offscreen_targets`.
- **Destruction is deferred, never blocking.** A dropped target goes to the pool's retired list and is
  destroyed once the frames that may still read it have rotated (`frames_in_flight`), exactly like
  a regrown pool target; `trim_offscreen_targets(true)` (the idle driver, once its GPU work has
  drained) destroys them at once. So a static UI still **holds no layer memory** after
  `WindowOptions::offscreen_idle_seconds`: the first change afterwards re-records (one miss), and the
  frame after hits.
- **Allocation.** The entry vector and the saved damage vector are reserved at construction; a
  frame that hits or refuses allocates nothing. Creating the target of a record is a device
  allocation, once per change of a node, not per frame.

## 6. Damage and dirty rendering

[`../dirty-rendering.md`](../dirty-rendering.md) section 5 says a clean subtree costs no walk; a
retained layer extends that to the GPU. The integration is:

- A hit composites only `entry region ∩ clip ∩ damage regions`; a damage region that misses the
  layer records no pass and no draw.
- The pixels outside the damage are the frame target's, untouched, as for any damaged frame.
- A record under damage still renders the whole region, so a damage-clipped frame can be the first
  frame of a later full one. The cost is one-time per change.
- A filtered node's damage already includes its halo (`add_filter_dependencies`); the entry's region
  includes the same halo, so a hit and a fresh render cover the same pixels.
- A partial repaint through `ClippedPaintSink` still cannot layer (it paints a group flat), so
  retention only pays on full repaints and on sinks that layer under damage (`begin_damaged` with
  a `Batched2DSink`); the pipeline gives retention to full repaints only and bumps the epoch on the
  others, so a layer is never trusted across a frame it could not see.
- The layer box is the node's subtree paint box (`Node::subtree_paint_bounds`, the superset the cull and
  the damage pass already rely on), not the whole frame as the unretained layer still uses; an
  unbounded subtree (text that may overflow) falls back to the frame.

## 7. Resize and device loss

- **Resize or format change.** `RetainedLayerCache::begin_frame` compares the frame extent and format and
  drops every entry (the regions are device pixels of the old frame); the frame-extent is also part
  of the key. Dropped targets wait out their frames like any retired target.
- **Content scale** (render-scale downgrade, DPI change) is in the key.
- **Device loss / recreation.** The cache lives in the renderer, which lives with the device. A lost
  device recreates both; nothing is carried across, and the first frames re-record. Nothing in the
  cache holds a handle that outlives its renderer.

## 8. Tests

- `layer_retain_null` (null backend): record then composite, counting passes, draws, sampled binds
  and copies against the uncached control; invalidation by a dirty node and by a key change, one
  entry per owner; the whole-box record under damage and the damage-cut hit; entry cap, byte cap,
  LRU, the pool's own cap, never evicting this frame's entries; a degraded layer; resize, trim and
  idle release at ring depths 1, 2 and 8; a filtered layer's region; recycling hazards stay clean.
- `paint_layer_retain` (unit): the question and the skip on yes; a dirty descendant, a moved node, a new
  epoch and a new opacity miss; retention off paints as before; culling never hides a dirty node;
  an isolated opacity group; the subtree draw list (recorded, replayed on yes and on no, recorded
  over a yes), plus a subtract/intersect mask: parts are skipped with a kept layer, replayed in
  balance on a sink that let go, skipped on a sink that declines them.
- `layer_retain_mask_null` runs a masked card with a subtract part: it records once, hits draw only
  the composite, a dirty neighbour next to a hit renders mask and part with balanced passes, and a
  kept layer refuses a mask.
- `layer_retain_gpu` (Vulkan or lavapipe): four scenes (gradient mask under opacity with overlapping
  children, blur, drop-shadow with a mask, isolated opacity group) over nine frames each - record,
  hit, two damage-clipped hits, a full frame, a descendant change, a re-record, a damaged hit - are
  byte-identical to the unretained paint, and the retained run really hit (6 hits, 2 records).
  Mutation-checked: dropping the entry's origin from the composite fails it.

## 9. Measured

`layer_retain_bench` (Release; 1920x1080; 16 cards, 12 with a gradient mask and opacity 0.9, 4 with
`blur(6px)`, eight children each; 300 steady full-redraw frames; Radeon RX 5300M with a blocking
submit, so the time includes the GPU). "Uncached" is the layer box of every layer being the whole
frame, as before; "tight box" gives each layer its subtree box but keeps no pixels; "retained" is
the cache.

| | passes | draws | CPU paint + record (null) | Vulkan frame |
|---|---|---|---|---|
| uncached | 65 | 65 | 0.287 ms | 10.4 ms |
| tight box | 65 | 65 | 0.289 ms | 4.1 - 4.4 ms |
| retained | 17 | 17 | 0.052 ms | 0.85 - 0.89 ms |

Four cards: 17 -> 5 passes and draws, 0.076 -> 0.015 ms, 3.07 -> 0.51 ms. The tight box is a
separate, smaller win (the composites and clears no longer cover the whole frame); retention is the
rest. The 17 passes of the retained frame are the frame pass and one reopen per composite. Memory
of the 16-card scene: 16 targets of about 440x230 (about 7 MB together) against the frame-sized pool targets an
unretained frame needs live; after the idle window both are released.

## 10. Not done

- A change of only `opacity` re-records: it marks the node dirty, and the entry would still serve.
  Splitting "opacity-only dirty" needs a style-level signal the dirty flags do not carry.
- Hysteresis: a node that flips between dirty and clean every other frame records every time it is
  clean (one copy and one target creation each). A node dirty on every frame never records.
- Layers under `ClippedPaintSink` (partial repaints) are still flat; see the `filter-damage` and
  layer work in section 5.2.
- Retained text layers do not report glyph-atlas pressure (section 4).
- WebGPU and Metal: the cache uses `copy_target_region` and the mode-2 composite, both already
  implemented there; it is exercised by the null and Vulkan lanes only.
