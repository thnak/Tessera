# Path model and rasterizer seam

> [<- Setup and pin](01-setup-and-pin.md) | [Measurements and Vulkan viability ->](03-measurements.md)

## 3. The PathRasterizer seam

Public API (no third-party include in any public header):

- [`path_model.hpp`](../../../include/tessera/rendering/path/path_model.hpp): `PointF`, `Rgba8`,
  `FillRule` (non-zero/even-odd), `LineCap` (butt/round/square), `LineJoin`
  (miter/round/bevel), `PathSegment` (`move_to`/`line_to`/`quad_to`/`cubic_to`/`close`), `Path`,
  `StrokeStyle` (color, width, cap, join, miter limit), `PaintStyle` (optional fill + optional
  stroke), `PathShape`, and `RasterTarget` (RGBA8, straight alpha, explicit `width`, `height`,
  `stride_bytes`; caller-owned pixels).
- [`path_rasterizer.hpp`](../../../include/tessera/rendering/path/path_rasterizer.hpp):
  `PathRasterizer::rasterize(std::span<const PathShape>, const RasterTarget&)` returning
  `std::expected<RasterStats, RasterError>`, plus `path_backend_available()` and
  `make_path_rasterizer()` over `PathBackend::{null_backend, thorvg}`.

Implementations live in `src/rendering/path/`:

- `path_rasterizer.cpp` - backend factory and the null/stub backend (validates the request,
  paints nothing, always links). Non-ThorVG builds compile only this file.
- `thorvg_path_rasterizer.cpp` - the **only** Tessera file that includes `<thorvg.h>`.
- `path_validate.hpp` - shared path/paint validation so every backend rejects the same inputs.

Mapping contract used by the ThorVG adapter:

| Seam | ThorVG v1.1.3 |
|---|---|
| `move_to`/`line_to`/`cubic_to`/`close` | same-named `tvg::Shape` calls |
| `quad_to` | exact promotion to `cubic_to` (ThorVG has no quadratic command): `c1 = from + 2/3(control-from)`, `c2 = end + 2/3(control-end)` |
| `FillRule::non_zero`/`even_odd` | `tvg::FillRule::NonZero`/`EvenOdd` |
| stroke width / color / cap / join / miter limit | `strokeWidth`/`strokeFill`/`strokeCap`/`strokeJoin`/`strokeMiterlimit` |
| straight-alpha RGBA8 buffer | `tvg::SwCanvas::target(..., tvg::ColorSpace::ABGR8888S)`; on little-endian hosts the byte order is R,G,B,A and the values are un-premultiplied |
| shapes composited in order | one `SwCanvas`, shapes added in order, `draw()` + `sync()` |

Canvas and shape ownership follows ThorVG's model: a successful `add()` transfers the paint to the
canvas, and deleting the canvas releases every paint, so one rasterize call is
create-add-draw-sync-delete with no leaks (the spike runs repeatedly in the golden test).


## 4. Determinism

Determinism is a hard test requirement ([ADR-009](../../../decisions/ADR-009-deterministic-headless-golden-tests.md)),
so the adapter:

- initializes ThorVG with `tvg::Initializer::init(0)`: **zero worker threads**, so rasterization
  runs inline on the calling thread and cannot vary with scheduler timing;
- has no wall clock, no fonts, no random ids, and fixed float geometry in the spike scene;
- writes straight-alpha RGBA8, so the PNG bytes are the rasterizer bytes (no compositing fixes
  applied afterwards).

Evidence (same machine, same build):

- two runs of the Release spike: identical raw RGBA (`SHA-256
  7579c3e5bda7a5336b28b7eff633498ff3b1fb50acae1ef99361310845ccd999`), identical PNG (`SHA-256
  1dedd7c021fd18228840591a7a09315e9dfbea9741d37784716e6866ecf0f20d`), identical FNV-1a
  `0xc401671319a3669e`;
- a Debug build and a Release build of the same source produce the same PNG and the same raw
  buffer byte-for-byte, so the golden does not depend on the build type.

The checked-in golden is `tests/golden/thorvg/icons.png` and CTest runs
`thorvg_spike_golden` (`--compare` with zero tolerance). The scene covers every seam feature:
rounded-rect fill, miter stroke, cubic curves, quadratic curves, both fill rules (the even-odd
pentagram/donut are visibly different from non-zero), stroked-only paths, all cap styles, all join
styles, and a translucent overlay that exercises straight-alpha blending over existing content.
