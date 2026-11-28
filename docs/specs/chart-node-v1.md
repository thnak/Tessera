# Chart nodes v1

**Status:** implemented (issue #183). The sample storage this spec builds on is
[`chart-series-v1.md`](chart-series-v1.md) (issue #182).
**Scope:** the hybrid chart node: retained plot geometry derived from
[`ChartSeriesRing`](../../include/tessera/ui/chart/chart_series.hpp), the damage a rebuild reports,
the immediate-mode escape hatch, and the frame-budget metrics the diagnostics overlay shows.

This document is normative for the chart node's paint and damage contract. Rendering *quality*
(antialiasing, joins, curves) is not specified here: V1 emits axis-aligned boxes and says so.

## 1. Why a chart node is not a scene node

`ChartNode` is deliberately **not** a subclass of `ui::Node`, and it does not attach a payload to one.

1. `include/tessera/ui/scene/node.hpp` is at its source-size baseline and must not grow. A chart needs
   no new node capability: no layout input (the owner computes the plot rectangle), no hit testing
   (V1 has no interaction), and no per-node style beyond what the owner already resolves.
2. A chart's repaint is decidable from its own geometry. Routing it through a node would make every
   box a paint-pass concern, and the paint pass would have to know about series, samples and plot
   windows to decide what to emit - the coupling the dirty-rendering design explicitly avoids.

The chart therefore borrows a **plot rectangle** in absolute device pixels - the same space
`PaintSink` draws in - and the owner that knows the node's box computes it. Hit testing, focus and
accessibility stay on the node; the chart is the thing painted inside it.

## 2. Coordinate space

`ChartPlot` maps two data domains to a rectangle:

- `rect` is absolute device pixels, not node-local. The owner applies the node's world transform by
  painting through a sink that is already transformed, exactly as any other content.
- `time` is the x domain (`ChartTimeRange`, seconds on the series' own monotonic clock) and `value`
  the y domain (`ChartValueRange`). x increases to the right; y is **inverted** because a larger
  value is higher on screen.
- A domain whose `max <= min` is *disabled* and means auto-fit (section 3). A disabled domain maps to
  the plot's leading edge rather than dividing by zero, so a one-sample or flat series still produces
  a determinate box.
- Samples outside the domain clamp to the edges. A chart that wants clipping semantics clips at the
  sink, not by dropping samples.

## 3. Ranges

| Configuration | Behavior |
| --- | --- |
| `set_time_range` / `set_value_range` with `max > min` | The window is pinned. New samples outside it clamp; nothing is rescaled. |
| `set_time_range` / `set_value_range` with `max <= min` | Auto-fit: the domain is resolved from the stored samples on the next rebuild. |
| `set_auto_ranges` | Both domains return to auto-fit. |

Auto-fit resolves `time` to `[oldest, newest]` across every series and `value` to the observed
`[min, max]`. **An auto-fit domain that changes invalidates every retained segment**, because every
sample moves on screen; the damage is then the whole plot (section 5). A live chart that wants a band
repaint pins its window - that is the recommended configuration, and it is what the damage tests use.

## 4. Retained geometry

One `ChartSeriesGeometry` per series, sized to the series capacity at construction:

- `segments[i]` is the box of the polyline segment between sample `i` and `i + 1`.
- `markers[i]` is sample `i`'s marker box (empty when `marker_size` is 0).

**Segment box.** The axis-aligned bounding box of the two endpoints, inflated by half the stroke
thickness on every side. Horizontal and vertical segments are therefore exact. A diagonal segment
covers its whole bounding box: this is the V1 approximation, and it is deliberate - it is a superset
of the stroked segment, so damage computed from it is never too small, and it costs one rect instead
of a polygon. A real stroked primitive is a renderer change, not a spec change.

**What invalidates retained geometry:**

| Event | Rebuilt |
| --- | --- |
| Append into a ring that is not full | Only the segment ending at the new sample. |
| Append that evicts the oldest sample (ring full) | Every segment: eviction renumbers every logical index. |
| Plot rectangle change; explicit range change; auto-fit domain change | Every segment. |
| Stroke thickness or marker size change | Every segment. |
| Series color change | Nothing: colors are read at emission time. |

`rebuild()` is the single entry point: it resolves the domains, rebuilds only what is dirty, and
returns the damage. A `rebuild()` with nothing dirty does no work and reports no damage.

## 5. Damage

`ChartDamage` is a bounded, ordered set of rectangles (`kMaxRects = 8`).

**The rule** is the per-node rule of [`dirty-rendering.md`](../design/dirty-rendering.md) applied per
segment: for every rebuilt box, the **union of its old box and its new box** is added. A segment that
moved by a few pixels therefore damages a band, and a segment that did not move damages its own box.

- Rectangles that touch are AABB-merged, exactly like `rendering::DamageAccumulator`.
- Exceeding the cap replaces the set with the **plot rectangle**. That is over-damage only; the chart
  never reports less than it changed.
- A whole-plot rebuild reports the plot rectangle, because the boxes it replaces are no longer
  individually known. A chart that has never painted reports nothing (the owner's first frame paints
  it whole regardless).
- `full()` reports the overflow case so a caller can log it.

**The chart owns no redraw mechanism.** Damage flows out through the caller:
`ChartDamage` -> `rendering::DamageAccumulator::add` -> `make_redraw_plan` -> the existing
`ClippedPaintSink` / `Batched2DRenderer::begin_damaged`. Section 9 pins that path.

## 6. Emission

`paint_chart(chart, sink, opacity)` emits every non-empty retained box as one `fill_rect`, segments
before markers, in series order. `opacity` folds into each series color's alpha; a series that becomes
fully transparent emits nothing at all.

Damage clipping is **not** reimplemented in the chart: painting through `paint::ClippedPaintSink`
drops every box outside the plan's regions, which is the same sink `FramePipeline` uses for all other
paint content. A full paint and a damaged paint therefore produce the same boxes in the same order,
and the sink decides which of them reach the target - that equality is what makes dirty-only and full
output equivalent (section 9).

One `fill_rect` per box is the V1 *engine* choice: a chart's cost is dominated by how many boxes survive the
damage filter, not by batching the ones that do. It is not the drawing model of the chart components:
section 11 records that they draw polylines and arcs as `PathShape`s through the path seam.

## 7. The immediate-mode escape

The hybrid contract has two halves. Retained geometry covers the data; a chart inevitably also grows
annotations, threshold bands, brush selections and reference lines that no retained box set expresses.

`ChartImmediateFn` is one plain function pointer plus a caller-owned `void*` context. It runs **after**
the retained pass, so an annotation draws over the plot, and it receives:

- the sink being painted through - which may be the damage-clipped wrapper, so an immediate draw is
  clipped exactly like a retained one and needs no damage logic of its own;
- the resolved `ChartPlot`, so an annotation uses the same mapping as the data;
- the effective opacity.

The hook must not retain the sink or the plot. It is not `noexcept`: exceptions belong to the caller's
boundary. A null hook is the default and costs one branch.

## 8. Frame-budget metrics

`diagnostics::FrameBudgetRecorder` publishes the frame budget beside
`AdaptiveQualityController::budget_ms()`, which remains the single source of the budget value
(`frameBudgetMs`, else `1000 / target_fps`; 0 means no budget).

| Metric | Kind | Unit |
| --- | --- | --- |
| `frame.time_us` | gauge | microseconds |
| `frame.budget_us` | gauge | microseconds |
| `frame.budget_overruns` | counter | count, cumulative |

Units are microseconds because the registry stores integers; the overlay prints milliseconds, the unit
the budget is configured in. Names follow the project's lowercase dotted convention.

**Overrun rule.** A frame is an overrun when a budget is configured (`budget_ms > 0`) **and**
`frame_ms > budget_ms`. A frame exactly on the budget is inside it. With no budget, nothing is ever an
overrun and the overlay omits the line entirely rather than printing a ratio against zero.

`OverlayModel::build` gains one line, between the damage summary and the metrics:

```
frame 12.0ms  budget 16.7ms  ok  overruns 0
frame 21.3ms  budget 16.7ms  OVER  overruns 3
```

The verdict is decided on the numbers, not the printed text, and the two summary lines are unchanged.

## 9. Acceptance

- `tests/unit/chart_node_tests.cpp` (+ `chart_node_allocation.cpp`, CTest `chart_node`): domain
  mapping and clamping, box construction, the damage set (merge, keep-apart, overflow fallback,
  clear), single-append band damage, auto-fit whole-plot damage, eviction re-damage, clean rebuild as a
  no-op, rejections, range and invalidation rules, emission order and opacity, the immediate hook, and
  **zero allocations** on the per-frame path measured by replaced global `operator new`/`delete`.
- `tests/gpu/batched2d/chart_node_damage_tests.cpp` (CTest `chart_node_damage`): one appended sample
  produces a damage rectangle **strictly smaller than the node's plot**; the shared accumulator and
  policy turn it into one scissor; every chart draw is inside the plan's region; the partial frame
  emits strictly fewer vertices and boxes than a full repaint.
- `tests/gpu/batched2d/chart_node_equivalence_tests.cpp` (CTest `chart_node_equivalence`): the
  dirty-only repaint on the deterministic CPU canvas is byte-identical to a fresh full redraw, with a
  guard proving an undersized region does leave stale pixels (so the equality is not vacuous).
- `tests/unit/frame_budget_tests.cpp` (CTest `frame_budget`) and
  `tests/unit/diagnostics_overlay_tests.cpp` (CTest `diagnostics_overlay`): registration and kinds,
  the overrun rule, rebinding, and the overlay's exact budget lines.

## 10. Non-goals

- Line/bar/area/scatter variants, axes, ticks, legends and grids: later chart-type work (#184). V1 has
  one chart type: a polyline drawn as boxes, with optional markers.
- Decimation and downsampling: [`chart-series-v1.md`](chart-series-v1.md) section 8 leaves the policy
  open; V1 maps every stored sample.
- Antialiasing, joins, caps and curves: V1 boxes are axis-aligned and hard-edged. Strokes and arcs
  come from the path seam (section 11), not from a new `PaintSink` primitive.
- Interaction (hover, tooltip, zoom, selection): component library work after #183.
- Screen-reader exposure of chart data: the owner node carries it; the chart paints.

## 11. Drawing model (decided, issue #531)

Decided by the owner on 2026-11-18 and recorded in
[ADR-057](../../decisions/ADR-057-vector-paths-thorvg-default-and-exit.md); no new ADR is needed.

- **Polyline (Sparkline, Line, Area).** A `PathShape` (`move_to` then `line_to`, round or miter join,
  optional closed fill for Area) rasterized by `PathRasterizer`. No polyline method is added to
  `PaintSink` or `Batched2DRenderer`, and no chart builds its own triangle strip.
- **Arc (Pie, Donut, Gauge).** A closed `PathShape` of `cubic_to` quarter arcs. `PaintSink` has no arc
  or ellipse primitive, deliberately: arcs go through the same seam. A pie is a donut with a zero
  inner radius.
- **Bar and markers** stay on `fill_rect` / `fill_rounded_rect` (axis-aligned, no path needed).
- **Compositing.** ThorVG is CPU-only, so a path chart rasterizes to RGBA8 and uploads through the RHI
  like `Icon`; the retained box set above remains the damage and equivalence model.
- **Limitation, `filter` and `clip-path` are `Later`**
  ([`stylesheet-v1.md`](stylesheet-v1.md) section 11). A chart cannot have a glow, a drop shadow on a
  line, or a clipped plot area; clip the plot with the rectangular `overflow` clip, and draw nothing
  outside the plot box. Component specs must not promise these.

### Measurements

Linux x86-64 (28 threads), GCC, Release, ThorVG via the seam, 220 runs (first 20 discarded), RGBA8
white target, round cap, stroke width 2. Source: a throwaway harness over `make_path_rasterizer`
(the numbers are machine-dependent; the shape of the curve is the finding).

| Case | Target | Median | p95 |
| --- | --- | --- | --- |
| Polyline 100 points, round join | 800x300 | 0.69 ms | 0.94 ms |
| Polyline 1,000 points, round / miter | 800x300 | 10.9 / 8.8 ms | 10.9 / 8.8 ms |
| Polyline 10,000 points, round / miter | 800x300 | 285 / 231 ms | 287 / 260 ms |
| Area (1,000 points, fill + stroke) | 800x300 | 14.5 ms | 15.5 ms |
| Donut, 6 slices (cubic arcs) | 300x300 | 0.18 ms | 0.21 ms |
| Pie, 6 slices | 300x300 | 0.19 ms | 0.20 ms |

Every case was byte-identical across all 220 runs (the ADR-057 determinism trigger did not fire).
The test signal is deliberately hostile (a fast ripple on a slow wave, so segments are long and
overlap); a smooth series is cheaper, but the scaling is not: cost is about 16x for 10x points from
100 to 1,000 and 26x again for 10x more.

Consequences, binding on the chart components:

1. **Arcs are cheap, polylines are not.** Pie, donut and gauge fit any frame budget.
2. **Decimate before emission.** A polyline must be reduced to at most about two points per device
   pixel of plot width (min/max per pixel column) before it becomes a `PathShape`. At 800 px that is
   at most 1,600 points; raw 10,000-point series at 285 ms miss a 16.7 ms frame by 17x. The policy
   itself is the open question of [`chart-series-v1.md`](chart-series-v1.md) section 8 and belongs to
   the Line component work.
3. **Raster once, not per frame.** The path result is cached as a texture
   ([`06-texture-cache.md`](../design/thorvg-spike/06-texture-cache.md)) and re-rasterized only for the
   damaged column band when a sample is appended, so streaming does not pay the full cost per frame.
4. Under ADR-057 decision 3 this is **not** a cost trigger (the trigger is the spike's own
   envelope for a static icon set). It is the input to the existing
   [ADR-071](../../decisions/ADR-071-gpu-compute-tile-rasterizer.md) question for dense scenes.

### Order and parity page

The chart track is built in this order: **Sparkline, Line, Area, Bar, Pie/Donut**, with axis, grid,
legend and crosshair written once as shared parts, not per chart. The streaming dashboard sample
(#184, V1-M6) is the chart track's parity page.
