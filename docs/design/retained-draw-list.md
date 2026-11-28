# Retained draw list per clean subtree

Status: implemented (issue #476, slice 1). Part of [dirty-rendering.md](dirty-rendering.md) section 5.1.

`ui::paint::SubtreePaintCache` (`include/tessera/ui/paint/subtree_paint_cache.hpp`,
`src/ui/paint/subtree_paint_cache.cpp`) sits above the per-node [`PaintCache`](dirty-rendering.md#54-per-node-paint-cache-implemented-pipeline-owned).
When a node and everything under it is clean (`Node::subtree_dirty()` is Clean), the subtree's sink calls,
pops included, are kept as one flat op list. The next full repaint with the same root key replays that list
and never visits the subtree.

## Contract

- **Byte-identical.** A replay emits exactly the calls a walk would; dropping the store (budget `0`,
  `FramePipeline::invalidate_paint_cache`) cannot change the output. `subtree_paint_cache_tests` and the
  per-node cache tests assert it.
- **Key.** The root's `PaintCacheSignature` (geometry, sink-space mapping, clip stack, inherited opacity,
  context) plus the cull rect, because a culled paint emits fewer draws. Descendants are covered by the same
  dirty contract the per-node cache relies on: every paint-visible mutation marks Paint on the node and its
  ancestors, and `end_frame` clears the flags after the paint.
- **Only full repaints record and replay.** A partial repaint culls to a damage box that differs every frame,
  so a list keyed on it would never hit. A partial repaint still receives the store and drops the list of every
  dirty subtree it visits (`PaintContext::retain_draw_list = false`), so a later full repaint cannot replay a
  list that predates the change. A glass restyle caught this in the benchmark: a stale root list hid the
  backdrop.
- **Outermost clean subtree.** The recording pass runs the subtree once with a recorder that also keeps pops
  (`PaintCache::Recorder(sink, record_pops)`); nested subtrees inside it record nothing of their own.
- **Tombstones.** A subtree shorter than 8 ops stores an empty entry, so it is not recorded again each frame.
- **Stats.** A replay adds back the draw counters and the culled counters, but not `nodes`: no style is
  resolved, like a per-node hit.

## Budget

`drawListCacheBudgetMb` ([runtime-config-v1.md](../specs/runtime-config-v1.md)), default 32 MB, 512
entries, deterministic LRU. `0` turns the store off.

## Measured

12 000-node benchmark scene, null RHI, MSVC Release: a full repaint of an unchanged tree takes **0.01 ms**
against about 5 ms for the walked one (`cpu_benchmark`, row "full paint, clean repeat"). It helps the frames
that repaint everything with nothing changed (expose, forced repaint, swapchain recreate); the one-edit frame
is a partial repaint and is unchanged (0.13 ms).

## Not done

Generic layer promotion (slice 2 and the deterministic promote criteria of the issue). The retained layer
cache of [#617](gpu/offscreen-layers-retention.md) already exists for masked, filtered and isolated-opacity
groups, and `layerCacheBudgetMb` now sizes it (`window_retained_layer_bytes`; `0` also zeroes the entry
count). Promoting arbitrary expensive subtrees is only worth building if a measurement shows rasterization,
not emission, as the bottleneck: the retained list removes the emission cost, and the 12 000-node full paint
is 5.7 ms with viewport culling ([performance.md](performance.md)).
