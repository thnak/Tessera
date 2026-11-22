# Cache decisions: shaped run and pipeline

Status: both measured, neither built. Issue #706, [memory-model-v1](../specs/memory-model-v1.md)
sections 5 and 10.

## Why this document exists

The memory-model audit listed six bounded caches and found two missing:

| Cache | In code? |
|---|---|
| Glyph atlas (LRU) | yes, with eviction counter |
| Layout measure (`TextMeasureCache`, capacity 1024) | yes |
| Image/texture (`ImageTextureCache`, `max_bytes`) | yes |
| Path (`PathTextureCache`) | yes |
| Shaped run (font/size/features/script/text) | **not found** |
| Pipeline (refcount + LRU) | **not found** |

The issue's first work item is explicit: *"Measure whether they matter before building... Build only
the cache whose number is large."* This is that measurement and the decision it supports. A cache
built because a table said "not found" is a cache nobody justified.

## Shaped run: not needed, measured

**The finding that decides it: shaping is not reachable outside the measure cache.**
`FontTextMeasurer::measure` is the only caller of `HarfBuzzShaper::shape` in the tree
(`src/ui/text/font_text_measurer.cpp:21`, through the `shaped_advance` callback), and
`TextMeasureCache` wraps `measure`. So every shaping call is already behind a cache keyed on
(text, style, constraints).

`tests/benchmarks/memory/cache_probe.cpp` measures a scrolling list — 200 rows, a 20-row window
moving one row per frame, 60 frames — which is the worst case for a cache: every frame introduces
one new row and retires one.

```
measure_calls_total=79
measure_calls_per_frame=1
measure_cache_hits=1121
measure_cache_misses=79
measure_cache_entries=79
measure_cache_evictions=0
measure_micros_total=142
measure_micros_per_frame=2
shaping_calls_per_frame=1
```

**1 shaping call per frame, 2 µs per frame.** A shaped-run cache would sit between the measure cache
and the shaper, so it could only remove calls that the measure cache already collapses to one per
distinct key. The ceiling on its benefit is that one call: at most 2 µs per frame, and only for keys
that repeat across frames — which the measure cache already handles.

**Decision: not built.** The number is not large. Building it would add a second cache with its own
key, bound, eviction and invalidation rules to save at most 2 µs per frame, and the two caches would
have to be invalidated together or they would disagree about a font change.

The measurement is reproducible: `cache_probe --frames 60 --items 200`.

## Pipeline: not needed, measured

**The finding: pipelines are created once, at device setup, never on the frame path.**
`WindowDevice::ensure_presentation` (`src/app/window_device.cpp:169`) is the only creator of the
frame pipelines, and it runs when the presentation is (re)established — at startup, on a format
change, and on device recovery. `Batched2DRenderer` *receives* its pipelines
(`Batched2DRenderer(rhi::Device&, rhi::Pipeline&, ...)`, `set_texture_array_pipeline`,
`set_lcd_pipeline`); it never creates one.

So the steady-state pipeline creation count is **zero**, and a refcount + LRU pipeline cache would
have nothing to cache. The count is not small; it is zero.

**Decision: not built.** A cache for a resource created once per device generation is a cache that
never hits. The real pipeline cost is startup latency, which is a different problem with a different
fix (pre-warm, or a `VkPipelineCache` blob), and neither is what section 5's table asks for.

## What this leaves

The spec's section 5 table lists the shaped-run and pipeline caches as if they exist. They do not,
and this document is the record of why. The table is not edited to remove them: the spec describes
the target design, and the honest statement is "measured, not needed at the current numbers" rather
than "deleted from the design". If a future change makes shaping reachable outside the measure cache
(a shaped-run reuse across a font-size animation, say), the measurement is the thing to re-run.

## The long-run bound test

Section 10 asks for "long-run scroll/animation: bounded memory, no cache growth". That is
`tests/unit/cache_growth_tests.cpp`: it drives a scrolling list and an animation for many frames and
asserts the measure cache's entry count stays at or below its capacity and that its byte footprint
does not grow monotonically. It is a bound, not a benchmark: it fails on unbounded growth, not on a
slow frame.
