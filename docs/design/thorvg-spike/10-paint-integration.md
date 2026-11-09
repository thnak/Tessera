# Painting embedded vector assets (D10)

Status: implemented. This is the step that puts the [embedded asset bundle](09-assets.md) on screen:
a scene node can carry a `PaintKind::Vector` payload and `paint_scene` emits one textured quad for it
through the existing [document cache](08-document-cache.md) / `PathTextureCache` pipeline. Paint
rules live in [`docs/design/paint-pass.md`](../paint-pass.md) §11; this note records the contract,
the sink wiring and the measured cost.

## Payload contract

[`paint_data.hpp`](../../../include/tessera/ui/scene/paint_data.hpp):

- `PaintKind { Text, Vector }`; **exactly one payload kind is active** per node. `PaintKind`
  selects it, fields of the other kind are ignored (no "text over icon"), and `PaintData::empty()`
  follows the active kind. Text truncation semantics are unchanged.
- `PaintVectorStyle`:
  - `asset` — embedded asset name, **borrowed** view like the node metadata strings (generated code
    passes string literals). Names longer than `PaintData::kMaxAssetBytes` (256) are ignored, never
    truncated; normalization/lookup uses the bundle's shared rule (a malformed name is `not_found`,
    which is also a no-op).
  - `raster_width`/`raster_height` — explicit raster size in **device pixels** (0/0 derives from the
    content box and the sink device scale; one explicit axis derives the other from the document
    aspect).
  - `scale` — optional uniform multiplier on the sink device scale (0 = sink default). Explicit
    raster sizes win.
  - `tint` — multiplies sampled colors (white opaque default).
  - `opacity` — extra multiplier, clamped 0..1.
- Effective tint alpha = `tint.a * vector.opacity * computed opacity`; the paint pass folds all
  three before handing a `VectorDraw` to the sink.

## Emission contract (`paint_scene`)

`src/ui/paint/paint_vector.cpp` mirrors `paint_text_lines`:

- After the node's background and border edges, **inside** the node's own overflow/scroll clip,
  before its children (same ordering as text; glyphs and vectors are content).
- `rect` is the padded content box (border box inset by `padding-*`), in absolute coordinates.
- Zero-size content box, empty/over-long name, or zero effective alpha → nothing is emitted.
- `PaintStats::vectors` counts emitted draws.

`PaintSink` gains an **additive default no-op** `draw_vector(const VectorDraw&)` next to
`draw_text`, so recording/stub sinks and sinks without a vector backend keep compiling.

## Sink wiring and cache ownership

`Batched2DSink` takes a `VectorPaintConfig` by value at construction (constructor injection):

| Field | Rule |
|---|---|
| `bundle` | generated embedded bundle; borrowed, must outlive the sink |
| `documents` | `SvgDocumentCache*`; decode once per content hash, safe to share |
| `textures` | `PathTextureCache*`; one texture per (document, size, scale); caller must call `notify_device_lost()` before `Device::recover()` (D7), same as any cache owner |
| `device_scale` | default 1.0; multiplied into derived raster sizes |
| `texture_format` | must match the destination color space (`R8G8B8A8Unorm` for deterministic/UNORM targets, `R8G8B8A8Srgb` for sRGB targets, per D2) |

`draw_vector` then: bundle lookup → `load_asset_svg` (document cache) → compute the uniform
`scale_milli` from the requested raster size over the document width → `acquire_svg_texture`
(texture cache) → `Batched2DRenderer::push_texture` with the tint. Nothing throws and no allocation
per draw beyond what the caches already do.

Failure rule (documented, tested): **every vector failure is a silent no-op**, never a crash or an
exception. `VectorPaintStats` counts the outcomes (`draws`, `missing_assets`, `unavailable_backend`,
`failed_assets`, `missing_resources`). With ThorVG OFF the sink still draws the node's box visuals;
the vector payload becomes `unavailable_backend`. A missing/malformed asset paints nothing (the
node's background/borders still paint).

Framework/hot-path rule: `PathTextureCache` textures are destroyed on LRU eviction, so with more
than one frame in flight the cache must be sized to keep all in-flight frames' assets resident;
evicting a texture still referenced by a pending command list is a use-after-free. With the default
single frame in flight this is automatic.

## Damage-clipped replay

`ClippedPaintSink` (moved from `frame_pipeline.cpp` into
[`clipped_paint_sink.hpp`](../../../include/tessera/ui/paint/clipped_paint_sink.hpp)) replays
`draw_vector` once per intersecting damage region under a square region clip, exactly like
`draw_text`; the draw origin is unchanged, so pixels outside every region are not repainted.

## Measurements

Vulkan 1.0, `Radeon RX 5300M`, Debug build, 160×160 target, nodes sharing one asset
(`icons/icon_shapes.svg`, 64×64 document, one texture), `paint_vector_gpu_tests --measure`:

| Nodes | Cold frame (ms) | Warm paint (ms) | Warm frame (ms) | Decodes | Uploads | Texture hits |
|---|---|---|---|---|---|---|
| 1 | 9.13 | 0.118 | 0.866 | 1 | 1 | 10 |
| 10 | 2.60 | 0.455 | 1.29 | 1 | 1 | 109 |
| 100 | 6.12 | 4.48 | 5.45 | 1 | 1 | 1,099 |

- "Cold" is a fresh document+texture cache (decode + rasterize + upload), with the device already
  warmed after the first row.
- Sharing works as designed: N nodes cost **one** decode and **one** upload; every extra draw is a
  cache hit.
- Many distinct assets behave like the texture cache's LRU: one texture per (content, size, scale)
  up to the cache budget, then the least-recently-used texture is evicted (and re-rasterized if it
  comes back). Per-frame cost is dominated by the batched flush at these sizes, not the lookup
  (binary search, no allocation).

## CPU golden (follow-up slice, implemented)

The document cache and ThorVG rasterization are CPU-side and deterministic, but the *paint
composition* adds textured quads and blending that the original CPU canvas could not express. The
follow-up slice added exactly one bounded capability — a deterministic RGBA8 textured-quad blit in
`CpuCanvas` — plus a shared size/scale helper used by both the GPU and CPU sinks. The composed scene
now has a bit-exact CPU baseline (`batched2d_cpu_vector_golden`, tolerance 0) that runs on every
platform, and Linux CI enables ThorVG so it actually executes. Contract, scene and measurements:
[CPU vector paint golden](11-cpu-vector-golden.md).

## Tests

- `paint_vector` (unit): payload precedence (one kind only), emission order after background/before
  children, content box and padding, tint/opacity folding, size/scale passthrough, clip depth,
  damage-region replay, sink no-op counters for absent resources/missing/malformed assets and
  ThorVG OFF, and the documented cache-key/scale rule (derived and explicit).
- `paint_vector_gpu: real Vulkan readback on an embedded asset: pixels inside the content box
  (opaque fill and translucent circle over the background), background outside, two nodes sharing
  one decode/upload, missing-asset no-op, byte-identical repeat, sRGB target round-trip, validation
  enabled. In a ThorVG-OFF Vulkan build it asserts the documented no-op and returns clean.
- `paint_vector_gpu --measure` prints the table above.

## Deferrals

- Image formats beyond SVG (PNG/JPEG/WebP need the image stack; the bundle is byte-agnostic).
- Per-node `viewBox`/preserveAspectRatio, nine-slice scaling, and explicit tint gradients: the
  current contract is "document scaled into the content box, uniform tint".
- Vector animation (animated SVG), per-node vector hit-testing/alpha masks, and vector authoring
  hooks from the compiler: codegen can set `PaintData` later; nothing in the compiler emits vectors
  yet.
- Per-frame texture pinning for multi-frame-in-flight uses (see the cache lifetime rule above) and
  an async upload path; both are cache/scheduler work, not paint-contract changes.
