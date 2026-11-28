# ADR-057: Vector paths — ThorVG on by default, with a measured exit to an in-house rasterizer

- Status: Accepted (2026-11-18, owner). Amends [ADR-004](ADR-004-buy-vector-path-rasterization.md)
  decision 1 (the "do not write it in V1" rule now has a named exit condition) and the
  `TESSERA_ENABLE_THORVG` default in [`dependencies.md`](../docs/specs/dependencies.md).
- Date: 2026-11-18
- Related: [ADR-004](ADR-004-buy-vector-path-rasterization.md) (buy vector path rasterization),
  [ADR-003](ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin RHI and batched 2D renderer),
  [ADR-009](ADR-009-deterministic-headless-golden-tests.md) (deterministic goldens),
  [ADR-012](ADR-012-static-linking.md) (static linking),
  [`thorvg-spike/`](../docs/design/thorvg-spike/README.md) (the P1 spike and its measurements),
  [`library-farm-plan.md`](../docs/design/library-farm-plan.md) (the component and chart plan this
  unblocks), issues #530 (`Icon`) and #531 (chart drawing model)

## Context

[ADR-004](ADR-004-buy-vector-path-rasterization.md) decided to delegate vector path fill/stroke to a
production library behind a `PathRasterizer` interface, and rejected writing a rasterizer in V1 as
"high risk, low differentiation". The P1 spike then recommended **Go** with ThorVG, and left one
question open:

> The remaining decision for the Lead/owner is not Go/No-Go but **when** the option flips to ON by
> default; this spike keeps it OFF so the default build and its artifacts stay unchanged.

([`thorvg-spike/04-layering-scope-and-decision.md`](../docs/design/thorvg-spike/04-layering-scope-and-decision.md)
section 8.)

That question is now blocking real work. `Icon` (#530) is a stage-0 catalog item that does not exist
and five stage-1 components consume it; charts (#531) have their engine half built
(`chart_series.hpp`, `chart_node.hpp`, `chart_geometry.hpp`, `chart_render.hpp`, all tested) and no
component half. Both need paths, and `PaintSink` has no arc or ellipse primitive, so pie and donut
charts are blocked on the same decision.

The owner's direction (2026-11-18) is two-part:

1. **Turn ThorVG on.**
2. **If ThorVG does not measure up, implement our own path system inside the codebase, reducing the
   dependency over time.**

Part 2 is the part ADR-004 does not cover, and it is the part that needs a rule rather than an
intention: "if it does not work out" is not a trigger, and "reduce dependencies" is not a plan.

## Decision

### 1. `TESSERA_ENABLE_THORVG` defaults ON

The default build compiles ThorVG. The spike's numbers support it: a clean build of the spike adds
**12.2 s** (50 Ninja steps, 37 ThorVG translation units, `-j12`), the Release archive is
**894 KiB**, startup is **0.02-0.04 ms**, and a 512x512 frame of 15 shapes rasterizes in
**1.0-1.7 ms** ([`03-measurements.md`](../docs/design/thorvg-spike/03-measurements.md)). None of
those is the "big four" cost the dependency plan warned about.

The Linux CI lane already configures with `-DTESSERA_ENABLE_THORVG=ON` and runs the vector goldens
(`batched2d_cpu_vector_golden`, `codegen_vector_paint_golden`), so the ON configuration is exercised
on every push. Flipping the default makes the default build match the lane that already proves it.

### 2. The Windows lane keeps ThorVG OFF, explicitly

`.github/workflows/windows.yml` deliberately does not enable ThorVG: the FetchContent download plus
37 translation units land in a job whose 50-minute cap is already documented as tight (#258), and
the file says so in a comment. Flipping the default would silently add that cost to the lane.

The Windows lane therefore passes `-DTESSERA_ENABLE_THORVG=OFF` **explicitly**, and its two
`--allow-skip` reasons stay as they are. The lane/suite matrix in
[`scripts/check_release_suites.py`](../scripts/check_release_suites.py) is unchanged, because the
lane still cannot run those two goldens.

This is a deliberate asymmetry, not drift: **Linux proves the vector path, Windows proves the
default build without it.** Both configurations stay green, which is the property that matters when
the dependency is later removed.

### 3. The exit condition is measured, not felt

ThorVG is replaced by an in-house rasterizer when **any** of these is observed and recorded in this
ADR's follow-up section:

| Trigger | Measurement |
|---|---|
| **Determinism** | The same path set rasterized twice, on the same machine and across the two gating lanes, is not byte-identical, and the difference is not explainable by a documented rounding rule |
| **Correctness** | A path in the committed corpus renders wrong (fill rule, join, cap, miter, degenerate geometry) and the fix is not available in the pinned version |
| **Cost** | The dependency exceeds the spike's envelope by more than 2x on build time (12.2 s), Release size (894 KiB) or first-frame raster (1.7 ms at 512x512) |
| **Maintenance** | The pinned version cannot be upgraded for two consecutive release cycles because upstream breaks the seam, or upstream stops shipping the CPU engine |
| **Licensing** | The license stops being permissive or the static-link rule (ADR-012) cannot be met |

A trigger is not a reason to start writing a rasterizer on the spot. It opens a decision issue with
the measurement attached, and the owner decides.

### 4. The seam is the migration path, and it is already the right shape

`PathRasterizer` (`include/tessera/rendering/path/path_rasterizer.hpp`) is a pure-virtual interface
with `backend_name()` and `rasterize(std::span<const PathShape>, const RasterTarget&)`, plus
`PathBackend { null_backend, thorvg }` and `make_path_rasterizer()`. The path model
(`path_model.hpp`) is Tessera's own: `move_to`/`line_to`/`quad_to`/`cubic_to`/`close`, two fill
rules, a solid fill and a basic stroke. **No ThorVG type appears in a public header**, and a
configure-time gate fails the build if any file other than the two adapters includes a ThorVG
header.

An in-house rasterizer is therefore a **new `PathBackend` value plus one implementation file**, not
a rewrite: `PathBackend::internal`, `src/rendering/path/internal_path_rasterizer.cpp`, and the
existing seam, tests and goldens apply unchanged. The null backend already proves the seam can carry
a second implementation.

### 5. Dependency reduction is incremental and gated by the same goldens

The in-house rasterizer does not have to arrive all at once. The order that reduces the dependency
with the least risk:

1. **Fill only** (nonzero and even-odd), replacing ThorVG for icons and chart fills.
2. **Stroke** (caps, joins, miter limit).
3. **Curve flattening** at a documented tolerance, so a cubic is deterministic across platforms.
4. **Antialiasing** matching the existing golden tolerance.

Each step lands behind the seam with the vector goldens as the proof, and ThorVG stays available as
the reference implementation until the last step. The seam makes the two comparable: the same
`PathShape` list through both backends, diffed by the existing golden comparator.

## Consequences

### Positive

- `Icon` (#530) and the chart track (#531) are unblocked by one decision instead of two.
- The default build matches the Linux lane that already proves the vector path.
- The exit from ThorVG is a named, measured condition with a migration path that is one enum value
  and one file, not a rewrite.
- The Windows lane keeps its build budget and still proves the no-ThorVG configuration.

### Negative / risks

- The default build gains a network fetch at configure time. A machine without network access
  cannot configure the default build; `-DTESSERA_ENABLE_THORVG=OFF` remains the escape hatch and
  must keep working (it is the Windows lane's configuration, so it is tested).
- ThorVG v1.1.3 has **no Vulkan engine** (CPU, GL and WebGPU only), so V1 rasterizes on the CPU and
  uploads through the RHI. Large animated path sets will need the caching and tiling work the spike
  deferred; that is unchanged by this ADR.
- Two configurations (with and without ThorVG) must both stay green, which is more CI surface than
  one. The lane matrix already encodes it.

## Alternatives considered

- **Keep the default OFF and enable it per target.** Rejected: every consumer would repeat the
  decision, and the gallery, the library and the charts all need it.
- **Turn it on everywhere including Windows.** Rejected: it adds a network fetch and 37 translation
  units to a lane whose cap is already tight (#258), for two goldens that Linux already runs.
- **Write the rasterizer now, before shipping ThorVG.** Rejected: it is the multi-month correctness
  pit ADR-004 named, and it would delay `Icon` and the charts for no user-visible gain.
- **Skia Graphite instead.** Still the fallback in ADR-004 if ThorVG fails a trigger; not chosen
  now because its build is heavier and the spike already measured ThorVG as acceptable.

## Follow-up

- Flip the default in `CMakeLists.txt` and pin `-DTESSERA_ENABLE_THORVG=OFF` on the Windows lane.
- Update [`dependencies.md`](../docs/specs/dependencies.md) (status column and the ThorVG section),
  the `docs/README.md` status row, and the spike's section 8 to record that the default question is
  answered.
- Record the trigger table above in [`library-farm-plan.md`](../docs/design/library-farm-plan.md)
  section 6 so the farm knows the exit is a decision, not a task.
- When a trigger fires, append the measurement to this ADR and open the decision issue.
- **Chart measurement (2026-11-28, #531):** no trigger. Arcs rasterize in about 0.2 ms; a stroked
  polyline is 0.7 ms at 100 points, 11 ms at 1,000 and 285 ms at 10,000 (800x300), all byte-identical
  across 220 runs. Charts must decimate before emission; see
  [`chart-node-v1.md`](../docs/specs/chart-node-v1.md) section 11.
- **Cost trigger fired (2026-11-21, #683) for the large-raster use case:** a 4K ThorVG raster is
  14.9 ms (sparse) to 31 ms (dense) on the measured AMD machines, against a 16.7 ms frame. The decision
  is proposed in [ADR-071](ADR-071-gpu-compute-tile-rasterizer.md); on a faster CPU (2026-11-24) the same
  scenes take 5.2 and 9.2 ms, so the trigger is hardware-dependent.
