# Dials and gauges: conic gradients and masks, or the `Path` route

- Status: decided (2026-11-21). Issue #617, umbrella #618.
- Related: [ADR-003](../../decisions/ADR-003-thin-rhi-and-batched-2d-renderer.md) (thin RHI, batched 2D
  renderer), [ADR-004](../../decisions/ADR-004-buy-vector-path-rasterization.md) (buy vector path
  rasterization), [ADR-063](../../decisions/ADR-063-browser-matching-user-agent-defaults.md) (browser
  behaviour is the reference), [ADR-068](../../decisions/ADR-068-clip-state-in-a-storage-buffer.md)
  (clip state in a storage buffer), [`paint-pass.md`](paint-pass.md),
  [`background-layers.md`](background-layers.md).

## The question

#617 asks for one decision before any code: cover dials and gauges with `conic-gradient()` plus
`mask` and `filter`, or cover them with the `Path` element, and state what `Path` would need. The
reference draws knob arcs, tick marks and glow with a conic gradient masked to a ring plus
`drop-shadow`.

## What the engine has today

| Capability | State | Where |
|---|---|---|
| `linear-gradient()`, `radial-gradient()`, both `repeating-` forms | shipped, layered, golden-tested | `src/ui/style/gradient_*.cpp`, `src/ui/paint/paint_gradient.cpp` |
| `background-image` as a layer list | shipped | `src/ui/style/background_layers.cpp` |
| `box-shadow` (outer and inset), `text-shadow` | shipped | `src/ui/paint/paint_shadow.cpp`, `paint_text_shadow.cpp` |
| `backdrop-filter` (`blur`/`saturate`/`brightness`/`contrast`) | shipped | `src/ui/paint/` glass path |
| `clip-path` basic shapes | blocked on ADR-068 (Proposed) | — |
| `conic-gradient()` | absent | — |
| `mask` / `mask-image` / `-webkit-mask` | absent | — |
| `filter`, including `drop-shadow()` | absent | — |
| `Path` element | **asset raster only** | `src/ui/paint/paint_vector.cpp` |

The last row is the one that decides this. `Path` is not a live vector path: `PaintVectorStyle`
carries an **asset name**, a raster size, a scale and an optional tint, and the paint pass hands that
to the sink, which draws the pre-rasterized image. The `d` and `viewBox` props are parsed and stored
in the IR, but nothing rasterizes them at paint time. So "cover dials with `Path`" is not a smaller
job than conic gradients — it is a different, larger one.

## Decision

**Implement `conic-gradient()` first, then `mask`, then `filter`.** Do not route dials through `Path`
in V1.

The reasoning is that a conic gradient is the same shape of work the engine has already done twice.
`linear-gradient()` and `radial-gradient()` both reduce to "sample a stop list at a scalar and emit
solid bands through the shared sink hook", and the radial one already proved the pattern for a
gradient with no dominant axis: a bounded 32×32 tessellation in `paint_sink.hpp` that both sinks
call, so CPU and GPU emit identical geometry and one golden covers both. A conic gradient is the same
contract with `atan2` in place of the radius — one more `GradientValue::Kind`, one more tessellation
in the same header, no new shader, no new RHI surface.

`mask` and `filter` are the expensive ones, and they are expensive for a reason that is already
written down: ADR-068 records that `CommandList` has no storage-buffer binding and `CpuCanvas` has no
per-pixel mask composite. A mask is a per-pixel composite, so it needs the same seam work ADR-068
proposes, and `filter` needs it too (`drop-shadow()` is a blur plus an offset composite). That is why
they come after the conic gradient rather than with it: the conic gradient is shippable on today's
seam, and the mask is not.

## What `Path` would need, if it is ever the answer

Recorded so the option is not lost, and so a future decision has the list. A live `Path` is a
rasterizer, not a paint primitive:

1. **Rasterize at paint time.** Today the asset is rasterized ahead of time and the paint pass draws
   an image. A live path needs the geometry to reach ThorVG (or the CPU canvas) per frame, which
   means a raster cache keyed on the resolved geometry, or a per-frame rasterization cost.
2. **Gradient fill and gradient stroke.** The reference's arcs are gradient-filled. ThorVG supports
   both, but the `Path` props (`fill`, `stroke`, `strokeWidth`) are flat colours today, so the prop
   surface grows.
3. **Stroke caps and dash.** Tick marks are dashes. `stroke-linecap` and `stroke-dasharray` are not
   in the prop surface.
4. **Animation of the sweep.** A gauge's needle and its arc sweep are animated. That needs the
   geometry to be an animatable value, which is a different lowering from a string literal `d` — the
   IR stores `d` as written, and a per-frame `d` string would re-parse SVG path data every frame.
5. **A golden.** Goldens come from lavapipe and the CPU canvas only, so the CPU canvas needs the same
   rasterizer, or the path cannot be tested.

Items 1 and 4 are the ones that make this a project rather than a slice, and neither is needed for a
conic gradient.

## Consequences

- #617 becomes three landings in this order: `conic-gradient()` (plus the `repeating-` form), then
  `mask`, then `filter`. Each is its own issue-sized slice with a golden and a Chromium comparison,
  as the issue's "done when" asks.
- The `Path` element keeps its current asset-raster contract. Nothing in this decision changes it,
  and a project that wants a live path is a separate decision with the five items above as its
  starting list.
- `mask` and `filter` inherit ADR-068's blocker. If ADR-068 is rejected, they need a different seam
  answer before they can be scheduled; the conic gradient is unaffected either way.
- Dials and gauges stay out of V1-M1's remaining scope beyond the conic gradient. #618 is V1-M4, so
  the reference mock is not blocked by this decision — it is blocked by the conic gradient landing.
