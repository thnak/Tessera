# Offscreen layers: `filter` functions

Part of [`offscreen-layers.md`](offscreen-layers.md) (section 5.2, items 5d and 5g), split out to keep
that file under the documentation size limit. The pool, recycling and mask items it refers to (5c, 5e,
5f) stay in the parent file.

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
   standard deviation); the values are asserted by `layer_filter_gpu`. The other `filter` functions landed in 5g; still open: a CPU
   twin of blur and drop-shadow and the lavapipe golden.
5g. **`filter` colour functions (landed, issue #617).** `brightness()`, `contrast()`, `saturate()`,
   `grayscale()`, `sepia()`, `invert()`, `opacity()` and `hue-rotate()` need no RHI change and no
   pipeline. *Measured in Chromium 158* (`tests/fixtures/chromium_color_filter.hpp`, 530 opaque and 36
   translucent samples, `CSS.supports` and computed values): a function works on the straight-alpha
   colour (a translucent pixel stays `P = c' * a`; `opacity()` scales alpha only), grayscale/sepia use the
   0.2126/0.7152/0.0722 weights and saturate/hue-rotate the rounded 0.213/0.715/0.072 of the spec,
   **each function clamps its result to [0, 1] before the next** (a list is not one matrix:
   `brightness(2) contrast(0.5)` on white is 191, folded it would be 255), a negative argument is
   invalid, grayscale/sepia/invert/opacity clamp at 1 (the computed value of `invert(250%)` is 1), an
   omitted argument is 1 (so `grayscale()` is the full effect) and hue-rotate takes deg, grad, rad,
   turn or a bare 0. **Chain**: a `LayerFilterOp` of kind `ColorFunctions` carries up to three
   (id, amount) pairs in the storage of the other kinds (amounts in `offset_x`, `offset_y`, `sigma`,
   ids in `rgba`), so the 20-byte op and the chain's 64 bytes are unchanged; the paint pass packs adjacent
   colour functions three to an op and the style layer rejects a list that needs more than three ops.
   **Renderer**: `glass_blur.frag` mode 3 (the WGSL twin too) copies the layer 1:1 through the
   functions (ids in `radii`, amounts in `factors` of the unchanged 112-byte block) into the scratch
   target with the non-blending pipeline, and the scratch becomes the layer: one pass, no extra
   target beyond a blur's. `color_filter.hpp` is the shared reference (shader, CPU twin and tests).
   **CPU twin**: `apply_layer_color_filter` recovers the straight colour from the black/white pair.
   Evidence: `filter_color` (grammar and formulas vs Chromium, 1/255), `cpu_color_filter` (3/255),
   `layer_color_filter_gpu` (4/255, through paint -> sink -> shader), `layer_filter_null` (one pass).
5. **`mask-image`** (gradient images first: the engine already parses all three gradient shapes) as the
   layer's alpha multiplier, then `mask-mode`/`mask-size`/`mask-position`/`mask-repeat`, then `filter`
   (`drop-shadow()`, `blur()`, landed in 5d) as a layer post-process. Each with a Chromium comparison and a lavapipe
   golden, and the property-status table updated.