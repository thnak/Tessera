# Raster image assets (PNG/JPEG)

Status: implemented. Nodes can paint embedded PNG/JPEG assets, decoded from the
[asset bundle](09-assets.md) with the already-vendored `stb_image` (now production-linked) and
uploaded through a bounded texture cache. Raster images are the third content kind next to text
(`PaintKind::Text`) and vectors (`PaintKind::Vector`); all three share the same placement, clip,
damage and opacity rules. Paint rules are summarized in
[`docs/design/paint-pass.md`](../paint-pass.md) §12.

## Decode contract

[`image_loader.hpp`](../../../include/tessera/rendering/image/image_loader.hpp):

- Supported containers: **PNG and JPEG**, detected by signature (`89 50 4E 47…`, `FF D8 FF`).
  Anything else is `unsupported_format`; the loader never guesses or shells out.
- Output is tightly packed **RGBA8 straight alpha**, top-down, 4 bytes/pixel, deterministic: no
  threading, no wall clock, no filesystem access (bytes come from the bundle), and `stb_image` is a
  pure function of the input bytes.
- Caps (defaults; caller-overridable via `ImageLimits`): 8 MiB encoded input, 4096×4096 per axis,
  16 MP decoded raster. Every cap is checked before/after decode and reported as a value:
  `invalid_argument`, `input_too_large`, `unsupported_format`, `malformed`, `limit_exceeded`.
- 16-bit PNGs are rejected as `unsupported_format` in this slice rather than silently narrowed
  (see deferrals).

## Caches and texture key

- `ImageDecodeCache`: decoded RGBA8 keyed by the FNV-1a **content hash** of the encoded bytes, with
  a bounded LRU (64 entries / 32 MiB default). Hits return a shared, immutable decode; one image
  decodes once regardless of how many nodes draw it. A single image larger than the byte bound is
  returned but not retained.
- `ImageTextureCache`: one texture per **(content hash, raster width, raster height, format)**, LRU
  bounded (64 entries / 64 MiB), nearest resampling of the decoded pixels to the requested extent
  with the same integer pixel-center rule as `CpuCanvas::blit_texture`. Stable handles; D7 device
  loss clears every texture and bumps the generation, exactly like `PathTextureCache`.
- Size/scale comes from the shared D10/D12 helper (`resolve_image_raster` →
  `resolve_content_raster`): content box × device scale, explicit axis derives the other from the
  image aspect, `scale_milli` uniform over the image width. GPU and CPU sinks use the same helper.

## Colour-space rule (authored sRGB)

PNG/JPEG pixels are **authored sRGB** bytes. The texture cache is configured to match the target
color space, the same D2 rule as the vector path:

- UNORM target → upload `R8G8B8A8Unorm`; the authored bytes are sampled as-is and the UNORM
  attachment stores them unchanged. This is the deterministic golden path (and what the CPU golden
  reproduces exactly).
- sRGB target → upload `R8G8B8A8Srgb`; sampling decodes to linear, blending happens in linear space,
  and the sRGB attachment re-encodes, so the stored encoded bytes round-trip instead of being
  encoded twice.

Difference from the vector path: vector textures are **rasterizer output** (paint is applied in the
rasterizer's working space, and the same texture-format rule applies), while image bytes are
authored sRGB and must not be treated as linear on either kind of target. Uploading an image as
UNORM on an sRGB target would brighten it; uploading it as sRGB on a UNORM target would darken it.
The cache format must therefore always equal the target format (documented on both config structs).

## Paint contract

- `PaintKind::Image` with a `PaintImageStyle` payload: borrowed asset name (bounded by
  `kMaxAssetBytes = 256`, over-long names ignored), optional device-pixel raster width/height,
  optional uniform scale hint, optional tint and opacity multiplier. `PaintKind` selects exactly one
  active payload among text/vector/image; the other payloads' fields are ignored.
- Emission is identical to vectors: after the node's background/borders, inside its own
  overflow/scroll clip, before its children; `rect` is the padded content box; effective tint alpha
  folds tint × payload opacity × computed opacity. `PaintStats::images` counts draws.
- `PaintSink::draw_image` is an additive default no-op; `ClippedPaintSink` replays it once per
  intersecting damage region under the region clip, exactly like glyphs and vectors.
- Missing/corrupt/unsupported assets and an unconfigured sink are **documented silent no-ops**
  counted in `ImagePaintStats` (`missing_assets`, `corrupted_assets`, `missing_resources`,
  `texture_failures`). Paint never crashes because an asset is absent.

## CPU golden

`batched2d_cpu_image_golden` (`tests/gpu/cpu_image_golden_tests.cpp`, Vulkan-free executable
registered from `src/ui/paint/CMakeLists.txt`) renders the shared `cpu_image_scene.hpp` scene twice
(byte-identical repeat asserted), asserts one decode for four draws, and compares
`tests/golden/baseline/batched2d/dashboard_image_cpu.png` exactly (`--tolerance 0`). Scene: two
nodes sharing one PNG, a tinted/translucent node, a node clipped by an `overflow: hidden` parent and
a missing asset that stays background. **PNG only**: JPEG IDCT is floating point whose last bits may
differ across compilers, so JPEG is covered by decode unit tests and by the GPU readback test, not
by a cross-platform tolerance-0 golden.

Regenerate deliberately (never from CI):

```text
batched2d_poc --backend cpu --cpu-image --width 320 --height 160 \
  --out tests/golden/baseline/batched2d/dashboard_image_cpu.png
```

The same scene is available in the POC with `--backend cpu --cpu-image` for manual renders, and the
Vulkan readback test (`paint_image_gpu`) covers the GPU path: checker pixels inside/outside,
tint/opacity, missing-asset no-op, one decode for all nodes, one upload per (content, extent),
byte-identical repeat and the sRGB round-trip with validation enabled. `--measure` prints:

| Nodes | Cold frame (ms) | Warm frame (ms) | Decodes | Uploads | Texture hits |
|---|---|---|---|---|---|
| 1 | 9.0 | 1.53 | 1 | 1 | 10 |
| 10 | 2.3 | 1.89 | 1 | 1 | 109 |
| 100 | 6.1 | 5.08 | 1 | 1 | 1,099 |

(Windows Debug, Radeon RX 5300M, 160×160, 32 px nodes sharing one 8×8 PNG; cold = fresh caches.)

## Dependency and license status

`stb_image` moves from test-only to a **production-linked** dependency
(`docs/specs/dependencies.md` §1 row and the regenerated license bundle/SBOM): it is vendored
(`third_party/stb/stb_image.h`, public domain / MIT dual-licensed; the shipped bundle elects the MIT
option). The decode adapter is the only file that includes the stb header; no stb type appears in a
public Tessera header.

## Deferrals

- EXIF orientation (JPEG) and colour profiles (ICC) — bytes are decoded as authored, no transforms.
- 16-bit / HDR inputs, animated PNG/GIF/WebP/AVIF (stb's default set is deliberately restricted to
  PNG/JPEG here).
- Nine-slice scaling, tiling/repeat modes, per-node `object-fit` and mipmapped/minified sampling.
- Downscale-quality filtering (uploads use deterministic nearest), async decode/upload, and a
  per-frame texture-pinning policy for multi-frame-in-flight rendering.
