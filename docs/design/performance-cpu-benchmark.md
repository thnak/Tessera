# CPU benchmark and the render-thread trigger

Part of [Performance, power, and hardware utilization](performance.md) (section 9.1). Split out so
the hub stays under the 600-line limit; the section number is kept for existing references.

## Current measurement (2026-11-24)

Windows 11, 12 logical cores, MSVC Release (`vs2026-release`), 12 000 nodes, 7 measured runs after
2 warm-up runs; median and p95. Re-measured on `main` after #711 (per-generation style memo), #735
(delta `ComputedStyle`, retained Yoga tree), #793 and #817 (the `Node` split):

| Metric at 12 000 nodes | Median (p95) | Committed budget | #241 reference |
|---|---|---|---|
| First style + layout | 222.6 ms (240.6 ms) | 4500 ms | 739 ms |
| Full paint into draw lists | **5.7 ms** (6.4 ms), was 89.6 ms before #257 | 40 ms | 75 ms |
| Layout + paint after changing one text node | 19.2 ms (22.5 ms) | 1800 ms | 296 ms |
| Layout + paint after changing one container's size | 15.3 ms (19.6 ms) | 1800 ms | 296 ms |
| Full paint with one glass bar | 0.07 ms (warm cache, all clean) | 500 ms | - |
| Damaged-frame paint (one node) | 0.17 ms | 500 ms | 3.3-7.2 ms |
| Draw calls / quads emitted (of produced) / primitives | 19 / 40 (40) / 689 | 64 / 128 / 12 000 | 19 / 33 (65) / 9596 |
| Nodes painted by the full paint | 232 of 12 000 | 400 | 12 000 |
| Nodes painted by the damaged frame | 6 | 6 | 12 000 -> 5 |

What changed since the #241 reference: the incremental frames (one change, then layout and paint) went
from about 296 ms to 15-19 ms because the style memo, the delta style and the retained Yoga tree stopped
re-doing work for the unchanged tree, and the first frame is 3.3x faster. The full paint was 90 ms
(all 12 000 nodes) until #257, which made a full repaint cull to the viewport: it now paints the 232
visible nodes in 5.7 ms, 15x faster, and 1.4x the 4 ms trigger instead of 22x. The budgets are an
order-of-magnitude detector; the full-paint row is now tightened to 40 ms plus a node-count guard.
Allocations per frame are gated separately by `tests/golden/perf/alloc_budget.json` (issue #701,
[ADR-070](../../decisions/ADR-070-frame-arena-not-pmr.md)), per scene and scenario, with one table per compiler
lane. MSVC Release today: idle frame 0 allocations (1 until an empty interpolation view stopped building a `std::map` each frame), paint-dirty 16-32, layout-dirty 1 (203 until
the layout input tree was retained and rewritten in place, #701), and a
running animation 18 even when idle (`AnimationRuntime::tick` returns its result by value), against the
spec's "no malloc in a steady-state frame" in
[`memory-model-v1.md`](../specs/memory-model-v1.md). The 1 on MSVC and the animation rows are the
recorded gap to close, not a budget to relax.

**Full paint and #257 (decided: cull to the viewport, on by default).** A full-redraw plan now culls to
the whole target, using the one full-target scissor every plan producer stores
(`src/ui/frame/redraw_cull.hpp`), so `FramePipelineConfig::cull_paint` (default on, runtime key
`cullDraws`) covers full repaints as well as damaged ones. The prototype branch
(`perf/offscreen-paint-culling`, 34.75 -> 3 ms) predates #487 and #490 and is superseded by it: the
mechanism it added is already in `main`. Two details made the first measurement miss it. A dirty node
used to be painted whenever it was dirty, so the first frame of a scene (every node dirty, an empty
paint cache) culled nothing; it is now painted only while it has a cache record that could go stale,
and a subtree skip is refused while a dirty descendant has one. Limits: it does not help a genuinely
fully visible 12 000-node screen, so the render-thread question of ADR-021 stays open for that case.

## Reference measurement, issue #241 (historical)

The tables and measurements below are the original ones and are kept as a record of how each stage
was reached; the current numbers are in the section above.

`tessera_benchmark` (`tests/benchmarks/cpu/`, CTest name `cpu_benchmark`, issue #241) is the
GPU-free CPU benchmark behind the V1 single-UI-thread decision of
[ADR-021](../../decisions/ADR-021-async-runtime.md). It builds a deterministic 12 000-node scene
from a fixed generator (containers, text, raster-image nodes, scrolling regions and one virtualized
list that realizes 27 of 100 000 items) and drives the production headless frame path: style, Yoga
layout, scene-style sync, damage/redraw planning, and paint into draw lists. Every paint command is
recorded through the null RHI (`Batched2DRenderer`). There is no GPU, window, system font, network
or clock input: text measurement is the engine's deterministic stub, so the only machine dependence
is CPU speed.

Reference measurement (Windows 11, AMD Ryzen 5 4600H, 12 logical cores, MSVC 19.51, Release Ninja,
DPI scale 1, 7 measured runs after 2 warm-up runs; median and p95):

| Metric at 12 000 nodes | Reference median (p95) | Committed budget |
|---|---|---|
| First style + layout | 739 ms (757 ms) | 4500 ms |
| Full paint into draw lists | 75 ms (78 ms) | 500 ms |
| Layout + paint after changing one text node | 296 ms (301 ms) | 1800 ms |
| Layout + paint after changing one container's size | 296 ms (313 ms) | 1800 ms |
| Redraw plan regions (max of the three frames) | 2 | 8 |
| Draw calls / quads emitted (of produced) / draw-list primitives | 19 / 33 (65) / 9596 | 64 / 128 / 12000 |

The budget file is `tests/golden/perf/cpu_benchmark.txt`; each time budget is about six times the
reference median, which keeps roughly a factor of two on top of a presumed 3x-slower two-core
gating runner. It catches order-of-magnitude regressions and does not rank machines. Every run
prints the full table (times, damage count, draw calls, primitive count) into the CTest log, so CI
history shows the trend.

**The style pass after the security review (findings P1 and P3).** Two allocation defects dominated
the style stage, and both are now fixed:

- `PropertyRegistry::apply_initial_values` materialized every initial longhand through
  `ComputedStyle::set`/`contains`, each of which built a fresh `std::string` via
  `normalize_property`. One node's style cost roughly 250 heap operations before any cascade work,
  times every node on every style pass. The registry's own names are already normalized, so the
  pass now uses `set_normalized`/`contains_normalized` and skips the lower-casing copy (P1).
- `SelectorIndex::match` allocated a candidates vector, a `seen` `vector<bool>` sized to the whole
  index, and a result vector on every call, then deduped with a linear scan. It now writes into a
  caller-owned buffer and reuses a stamp array, so a repeated match allocates nothing (P3);
  `tests/unit/selector_match_tests.cpp` pins that with a replaced global `operator new`.

Measured on the reference machine above, same scene and preset: first style + layout went from
**449 ms to 384 ms** (a 14.5 % reduction; 222.6 ms today after #711 and #735), and the damaged-frame paint from 3.50 ms to 3.31 ms.

**The style representation (issue #735, slices E and F).** A `ComputedStyle` used to hold every
one of the ~100 longhands with an initial value as its own `std::string` pair (~6.5 KB per node), and
`perf` put its `lower_bound` and `memcmp` at **53 % of LLC misses** on a size-change frame. Now
`PropertyRegistry` builds one immutable `StyleDefaults` table from its own merged property list, a
computed style stores only the entries that differ from it (slice F), and every registered longhand
gets a stable small id in sorted-name order that indexes that table: one probe, one array read. The contracts are unchanged (`find` returns the effective value,
`contains` only what the style holds itself, `properties()` the full sorted effective style as a
view that allocates nothing, `identity()`/`cached_digest()` still live with the shared entries;
`tests/unit/style_delta_tests.cpp` pins each). With `frame_alloc_probe --computecheck`, one computed
style went from **99 allocations / 28 678 bytes to 43 / 11 848** at depth 2 and from **147 / 43 813
to 64 / 18 240** at depth 3, and a real computed style now holds **33 of its 101 effective entries**.

**Glass scenario (issue #244, [`glass-v1.md`](../specs/glass-v1.md) section 9).** The same full
paint with the reference dashboard's fixed top bar blurred (`backdrop-filter: blur(16px)` through an
inline style; no node is added, so the tree is still 12 000 nodes). The paint records one backdrop
composite - region plan, region copy, both `glass_blur` passes and the composite - through the null
RHI, so this is the **CPU record cost of glass**, the same quantity `glass_ms` reports
([`glass-v1.md`](../specs/glass-v1.md) section 8.1), not its GPU cost. Measured on the reference
machine in one run (median, p95):

| Metric at 12 000 nodes | Same run | Committed budget |
|---|---|---|
| Full paint into draw lists, no glass | 86.1 ms (88.0 ms) | 500 ms |
| Full paint with one glass bar | 86.7 ms (89.8 ms) | 500 ms |
| Backdrops recorded by the glass frame | 1 | 1 (exact) |

The composite costs about 0.6% of the paint, inside the paint's own run-to-run spread: it adds a
bounded number of commands (one copy, two fullscreen draws, two sampled binds) instead of per-node
work. Compare the two numbers of a single run rather than against the table above, whose rows were
recorded on an earlier, quieter machine state. `max_glass_paint_ms` therefore guards the same
order of magnitude as `max_full_paint_ms`: it catches a regression that turns glass into per-pixel
CPU work, not the GPU blur, which the exact-pixel tests and the (unbudgeted) GPU timings cover.

**CPU culling (issue #474, [`dirty-rendering.md`](dirty-rendering.md) section 6.4).** The same
reference scene produces 65 quads per full paint; with `Batched2DConfig::cull_draws` on (the
default) 32 of them cannot touch the viewport scissor and are dropped before staging, so 33 reach
the vertex buffer in 19 draw calls instead of 44 - the counters are deterministic, and a dropped
quad no longer splits a batch. The separate damaged-frame scenario (one text edit painted through
`begin_damaged`) measures the partial path: 1 damage region, 2 quads, 2 draw calls, and the damage
bounding box culls none of them, because the paint pass already clips emission to the plan's
regions. Measured on the reference machine (MSVC Release, median of 7 runs) in two runs: full paint
43.2 and 46.3 ms, damaged paint 44.0 and 46.7 ms. The paint is therefore still dominated by walking
the 12 000 nodes, not by staging their quads: the cull removes vertex work the GPU scissor would
have thrown away, and the traversal does not shrink. That is the argument for culling in the paint
pass itself, which #487 implements (measured next). `max_quads` and the new `max_damaged_quads` gate
the two counters, and `max_damaged_paint_ms` guards the damaged row at the same order of magnitude as
`max_full_paint_ms`.

**Paint-pass cull (issue #487, [`dirty-rendering.md`](dirty-rendering.md) section 6.8).** The damaged
frame above now shrinks its traversal: `FramePipelineConfig::cull_paint` (default on, from the runtime
`cullDraws` key) hands the paint pass the redraw plan's scissor bounding box, and a node whose painted
reach misses it skips its style read and its own emission. Measured on the reference machine (MSVC
Release, median of 7 runs, two consecutive runs each; the only difference between the arms is
`cull_paint`): damaged paint **45.5 / 45.0 ms -> 41.1 / 39.3 ms**, and its deterministic counter drops
from **12 000 to 9 353 painted nodes** (2 397 culled, 2 clip-bounded subtrees skipped whole, draw calls
2 -> 1), while the full paint, which never sets the rect, is unchanged (46.0 / 44.5 ms vs 45.4 /
44.4 ms). The time win is smaller than the node count suggests because 78% of the tree is text leaves:
an unclipped text payload can paint outside its own box (a wrapped line advances past the content
box), so a text node was only culled as part of a clipping subtree, and the nodes still painted were
exactly the ones a box test cannot bound. `max_damaged_paint_nodes` (10 000) gated the counter: a trip
back to the whole tree fails it.

**Text-fit hint (issue #490, [`dirty-rendering.md`](dirty-rendering.md) section 6.8).** That
conservative rule is now lifted where layout can prove it is unnecessary. `LayoutBox::text_fit` is a
three-state fact (`Unknown`/`Fits`/`Overflows`) the layout engine computes in `collect_boxes` from the
same inputs paint uses: the border box minus the style's `padding-*` longhands (not
`LayoutBox::content_insets`, which also counts the border), paint's own wrap decision
(`PaintData::wrap == PaintWrap::Wrap` AND `white-space` wraps, forwarded on `LayoutInput::text_wrap`),
and the engine's `TextMeasureCache`. `apply_layout` copies `Fits` onto the node as
`Node::text_fits_box()` (one flag bit, no `sizeof(Node)` growth), and `own_emission_bounded` trusts it
only for a payload that is still the plain text layout measured - no spans, no decoration, no
`max_lines`, no font-size/weight override - and only when paint's rect is at least the layout box.
Measured on the reference machine (MSVC Release, median of 7 runs): the damaged frame's deterministic
counter drops from **9 353 to 5 painted nodes** (11 721 culled, 2 clip-bounded subtrees skipped whole),
and damaged paint falls from 37.6 ms to **3.3-7.2 ms** across runs. The full paint is unchanged at
12 000 nodes, because the cull only applies to a damaged plan. `max_damaged_paint_nodes` is now **6**,
the smallest integer with headroom above 5: a platform difference in the culled set cannot fail the
gate, while a trip back to the whole tree still does.

**Texture arrays (issue #480 part 2, [`renderer.md`](renderer.md) "Texture arrays").** The batch key
is `(scissor, texture)`, so a texture change ends the batch. The issue gated descriptor indexing on
measuring that share first, and `texture_batch_measure` (`tests/gpu/batched2d/`, a manual tool, not a
CTest) does it: a 24-row dashboard - image, caption, image, body line per row - painted through the
production paint path, plus a same-geometry control arm where each image quad is a solid fill of the
same rect, so the quad count, vertex count and paint traversal are identical and only the texture
identity differs. Measured on the reference machine (RTX 4060, MSVC Release, steady-state second
frame):

| Arm | Draw calls | Vertices | Batch splits |
|---|---|---|---|
| interleaved, one set per texture | 144 | 6510 | 143 texture + 1 flush |
| interleaved, texture array | 1 | 6510 | 1 flush |
| control (images as fills) | 96 | 6222 | 95 texture + 1 flush |

**143 of 144 draw calls were texture switches (99%)**, against the issue's 19-draw-call reference
scene: that scene is the exception, not the rule, so part 2 was worth its complexity. The array path
is byte-identical to the one-set path (0 differing bytes of 1 228 800), which is the whole contract -
it changes how the same texels are addressed, never which ones. The command-recording time is
unchanged within the run-to-run spread (0.7-1.2 ms for the whole frame on both paths): the win is
draw calls and the per-draw descriptor bind, not CPU recording. `max_draw_calls` (64) in the committed
budget still gates the reference scene, which does not interleave textures.

**Trigger (2026-11-24: full paint 5.7 ms vs the 4 ms line, down from 89.6 ms by #257; still marginally exceeded).** Revisit the V1 decision of no render thread and no worker recording when
`paint_time_ms` exceeds about 4 ms or 30% of `frameBudgetMs` at 10k nodes. At 12 000 nodes the
recorded paint is 75 ms in the #241 measurement (43-46 ms for a full paint on the same machine in the
#474/#487 runs), about eleven to nineteen times the 4 ms trigger and 2.6-4.5x the 60 Hz frame budget
(16.7 ms), so the trigger is currently exceeded; issue #241 records the measurement and the V1
design is unchanged pending that discussion. The paint number is draw-list building only: glyph
rasterization is excluded because the benchmark runs without a font, and a full paint still paints
all 12 000 nodes because the paint-pass cull only applies to a damaged plan. "First style + layout"
includes the
full CSS cascade; a computed style is a delta over the registry's shared initial values (issue #735,
slices E and F), so 12 000 computed styles cost a few megabytes and layout and paint measure their
own work instead of re-copying ~90 default longhands per node. Do not claim parallelism without a
measured gain on this benchmark.
