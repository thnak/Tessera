# CPU vector paint golden

Status: implemented. The [vector paint integration](10-paint-integration.md) originally deferred a
CPU golden because the software canvas could not express textured quads. This slice adds exactly
that one missing capability — a deterministic RGBA8 textured-quad blit — and uses it to produce a
bit-exact `batched2d_cpu_vector_golden` on every platform, including Linux CI where lavapipe is not
required at all.

## Why this is enough

The CPU paint path only ever needed to composite the document's CPU raster. Everything upstream is
already deterministic and platform-independent:

- `SvgDocumentCache` / `load_asset_svg` decode static SVGs into `SvgDocument::shapes` (ThorVG
  geometry, painter order baked in).
- `PathRasterizer::rasterize(shapes, RasterTarget)` composites those shapes source-over into a
  caller-owned RGBA8 buffer on the CPU.
- `paint_scene` emits `VectorDraw` with the content box, tint and size/scale hints.

So the missing piece was a destination-side blit, not a rasterizer. `CpuCanvas::blit_texture` is
that piece: RGBA8 straight-alpha source rect, integer destination quad, UV/nearest mapping, tint
multiply, active-rect clip, and the canvas's existing integer source-over rounding. No filtering,
no new dependency, no framebuffer effects.

## Shared size/scale rule

`include/tessera/ui/paint/vector_raster.hpp` holds `resolve_vector_raster(draw, doc_width,
doc_height, device_scale)` and is used by **both** sinks:

- scale = payload `scale` or the sink device scale, clamped to `[0.01, 64]`;
- both raster extents 0 → content box × scale;
- one extent 0 → derived through the document aspect;
- explicit extents win, clamped to `[1, 16384]`;
- `scale_milli` = uniform raster width / document width, clamped to `[1, 16000]`;
- degenerate rect/scale/document → `nullopt` (documented no-op).

The GPU sink uses it for the texture key; the CPU sink uses the extents and `scale_milli` to
rasterize (via the shared `scale_path_shapes` helper) and the content box to blit. They cannot
drift.

## Scene and baseline

`batched2d_poc --backend cpu --cpu-vector --width 320 --height 160` builds a deterministic scene
through the normal paint pass: header/accent bars, two nodes sharing `icons/icon_shapes.svg`, a
`stroke_path.svg` node, a tinted + 50%-opacity node, an overflowing child *of* an `overflow:
hidden` parent (the child vector is larger than the clip, so the cut is covered by the golden), and a
missing asset node that must stay background. The baseline was regenerated in the raster-image slice
after the overflowing child was correctly attached to the clipping parent (it had been a root sibling,
so the clip was not actually exercised before). It prints paint stats plus `raster_ms`/`blit_ms` and the successful draw
count.

Baseline: `tests/golden/baseline/batched2d/dashboard_vector_cpu.png`
(SHA-256 `5397b4cecee21e19d1f69e5e507e0554641a2480872681dd4e8a3122b4a9b6bd`), compared by CTest
`batched2d_cpu_vector_golden` at `--tolerance 0 --max-mismatch 0`. Two runs are byte-identical (and
the PNG is deterministic: the CPU canvas has no wall-clock or GPU input).

Regenerate deliberately, never from CI:

```text
batched2d_poc --backend cpu --cpu-vector --width 320 --height 160 \
  --out tests/golden/baseline/batched2d/dashboard_vector_cpu.png
```

Inspect the PNG, run it twice to confirm identical bytes, then commit it with the behavior/test
change.

## Skip vs run rules

- Local ThorVG-OFF builds: the test is still registered but the scene prints
  `cpu-vector: skipped (ThorVG backend unavailable)` and exits 0; CTest marks it **skipped**
  (`SKIP_REGULAR_EXPRESSION`), so a default OFF build stays green without pretending to cover it.
- ThorVG-ON builds: no skip property, so the golden **must run and pass** at tolerance 0.
- Linux CI enables ThorVG explicitly (`-DTESSERA_ENABLE_THORVG=ON` in the `linux.yml` configure
  step) so the golden and the rest of the ThorVG suite (`thorvg_spike_golden`,
  `thorvg_svg_golden`, `svg_loader`, `svg_document_cache`, path/asset tests) actually execute. The
  release/package jobs keep the default OFF so shipped artifacts are unchanged.

## Measurements

Debug build, 320×160 scene, this machine (CPU path; no GPU involved):

| Metric | Value |
|---|---|
| CPU rasterize (5 documents, one per draw) | 1.19 ms |
| Textured-quad blit (5 draws, 64–96 px quads) | 0.95 ms |
| Emitted vector draws / successful blits | 6 / 5 (missing asset is the documented no-op) |

The raster dominates, as expected: one decode is shared by all nodes using the same asset, and the
blit is a per-pixel nearest-sample multiply-plus-blend.

## Deferrals

- Filtering/mipmapped CPU sampling (nearest only; the golden needs no filtering).
- CPU vector animation, hit-test alpha masks, and non-SVG image quad formats.
- A dedicated `goldens-thorvg` job if enabling ThorVG on the main Linux job proves too slow.
