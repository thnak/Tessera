# Chart drawing model: record the decision, measure it (#531)

## What changed
- `docs/specs/chart-node-v1.md` section 11: drawing model (polyline and arc are `PathShape`s through the path seam per ADR-057; bars stay on rects), the `filter`/`clip-path` limitation, the chart order (Sparkline, Line, Area, Bar, Pie/Donut, shared axis/legend/crosshair), #184 as parity page, and measurements.
- `component-library.md`, `library-farm-plan.md`: point at it; decision 3 no longer "open".
- ADR-057 follow-up: chart measurement entry (no trigger). No new ADR: the owner already decided (2026-11-18).
- `chart_render.hpp`: stale comment about a future renderer polyline primitive.

## Measurements (Linux, GCC Release, ThorVG, 800x300, 220 runs, byte-identical every run)
Polyline 100 pts 0.69 ms, 1,000 pts 10.9 ms, 10,000 pts 285 ms; area 1,000 pts 14.5 ms; donut/pie 6 slices about 0.19 ms.
Finding: arcs are cheap, polylines scale badly, so charts must decimate before emission and cache the raster.

## Not implemented, on purpose
A batched-2D polyline was not chosen, so there is nothing to implement; the issue says not to start component work.

## Verification
`python -B scripts/gates.py --quiet` 15/15; clang-format clean.

## What is left
Chart components (per-component issues), decimation policy (chart-series-v1 section 8), raster cache for streaming.

Closes #531
