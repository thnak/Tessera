# Paint cull: the measurement (issue #789)

- **Status:** Measured; the proposed cache is not needed
- **Date:** 2026-11-22
- **Issue:** #789 (split out of #711's one unmet criterion)
- **Refs:** #711, #701, #793, #800, #487 (the cull), #735

## What the issue claimed

`frame_alloc_probe` on `origin/main` `7bf5d079` measured a paint-dirty frame at **61** allocations
for 601 nodes against **25** for 31 nodes, and concluded that "the per-frame cost still has a term
proportional to the *scene*, not the damage". The proposed fix was a per-node two-bit cache
(`clips_children`, `opaque_subtree`) so the cull reads bits instead of resolving a style, turning
"201 resolves into 201 bit tests".

The issue's own first step was to measure those 61 allocation sites before writing the cache. This
is that measurement, and it does not support the conclusion.

## The measurement

`origin/main` `406f86d5`, Release, GCC 15.2, 100 measured frames after 20 warm-up:

| cards | nodes | allocs/frame | painted/frame | culled/frame | cull resolves/frame |
|---|---|---|---|---|---|
| 10 | 31 | 25.0 | 1.0 | 9.0 | **0.0** |
| 50 | 151 | 30.3 | 1.0 | 49.5 | — |
| 100 | 301 | 59.3 | 2.4 | 100.0 | — |
| 200 | 601 | **59.3** | 2.4 | 199.0 | **3.5** |

Three facts contradict the issue:

1. **The number is no longer 61.** #793 removed the per-node-per-frame style-diff allocation, which
   was 89% of a layout-dirty frame and a large part of this one. 601 nodes now measures 59.3, and
   the 31-node case is unchanged at 25.0.
2. **It does not scale with the scene.** 100 cards (301 nodes) and 200 cards (601 nodes) both
   measure **59.3**. Doubling the scene changes nothing, so there is no term proportional to scene
   size to remove.
3. **The cull is not the cost.** `culled/frame` is 199 at 601 nodes, but the cull's
   `styles.resolve` runs **3.5 times per frame**, not 199. The resolve is memoised and
   allocation-free, and it does not appear in the allocation-site list at all. The proposed two-bit
   cache would replace 3.5 memoised resolves with 3.5 bit tests.

## Where the 59 actually go

The allocation sites for a 601-node paint-dirty frame, by count:

| count/frame | site |
|---|---|
| 69 | (unresolved: the style cascade) |
| 23 | (unresolved) |
| 18.6 | `style::parse_color` |
| 11.5 | `text::bidi_apply_visual_runs` |
| 11.5 | (unresolved) |
| 7.1 | `paint::PaintCache::insert` (two sites) |
| 4.2 | `detail::split_commas` (two sites) |
| 4.2 | `paint::BackgroundLayerCache::get` (two sites) |
| 3.0 | `rendering::make_redraw_plan` |

These are **per painted node**, not per scene node: `painted/frame` is 2.4, and the counts track the
painted work (a colour parse, a bidi run, a cache insert per painted record). The plateau at 100
cards is the painted set saturating, not the scene.

## Why the cull is already cheap

`paint_node` culls 199 nodes but resolves a style for only 3.5 of them, because the two cull tests
short-circuit before the resolve:

- `may_skip_subtree` rejects a whole subtree with one test at its root (line 251), which is where
  most of the 199 come from — it returns before the resolve.
- `reaches_cull` then rejects individual nodes, and only a culled node **with children** reaches
  `styles.resolve` (line 263-266). In a card grid, most culled nodes are leaves.

So the comment the issue quotes ("a culled node with children pays the cascade anyway") is accurate
but describes a path that runs 3.5 times a frame, not 199.

## Decision

**Do not build the two-bit cache.** It would replace 3.5 memoised, allocation-free resolves with 3.5
bit tests, and the measurement says the remaining cost is per *painted* node — colour parsing, bidi,
paint-cache insertion — none of which a cull cache touches.

The issue's acceptance criteria are met by measurement rather than by code:

- *"A paint-dirty frame at 601 nodes costs allocations proportional to the damaged region"* — it
  already does: the number is flat from 301 to 601 nodes, and tracks painted nodes (2.4/frame), not
  scene nodes.
- *"The cull still makes the same skip decisions"* — nothing changed, so trivially yes.
- *"All goldens pixel-identical"* — nothing changed.

## What would actually move the number

If the per-painted-node cost is worth attacking, the sites are `parse_color` (18.6/frame),
`bidi_apply_visual_runs` (11.5) and `PaintCache::insert` (7.1). All three are *value* work on a
painted record, and the first two are candidates for the same treatment #793 gave the style diff:
retain the scratch instead of rebuilding it. That is a different issue from this one, and it should
start from this table rather than from the cull.

## How this was measured

The cull-resolve count is not exposed by the probe, so it was counted with a temporary inline
counter in `paint_pass.hpp` incremented at the `styles.resolve` call site, printed per frame, and
then reverted. The allocation sites come from `frame_alloc_probe --sites` (added in #793). The
numbers above are reproducible with:

```
./tests/benchmarks/memory/frame_alloc_probe --sites --only paint-dirty --cards 200 --frames 100 --warmup 20
./tests/benchmarks/memory/frame_alloc_probe --only paint-dirty --cards 10 --frames 100 --warmup 20
```
