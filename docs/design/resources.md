# Resources and asset pipeline

Status: design for Big Step B2/B3 (runtime slice and window/swapchain phases of [Big Step B](../../AGENTS.md#big-steps-group-work-dont-micro-step)). `src/rendering/resources/` is a placeholder; B1 uses
one-shot textures/buffers. Memory/ownership: [`memory-model-v1.md`](../specs/memory-model-v1.md);
buffer/tunable config: [`buffer-config-v1.md`](../specs/buffer-config-v1.md); lifecycle:
[`resource-lifecycle-v1.md`](../specs/resource-lifecycle-v1.md) and
[`startup-and-loading.md`](startup-and-loading.md).

Resources cover images, fonts, SVG/paths, shaders, themes, and GPU objects. The pipeline must
be deterministic, bounded, and asynchronous: loading never blocks the UI thread ([ADR-021](../../decisions/ADR-021-async-runtime.md), async runtime).

## 1. Asset kinds

| Kind | Source | Runtime representation |
|---|---|---|
| Image | PNG/JPEG/WebP (stb_image for PNG, libjpeg-turbo for scaled JPEG, libwebp for WebP, section 2) | GPU texture + sampler |
| Font | committed TTF/OTF | FreeType face + glyph atlas entries (text stack: [`text.md`](text.md)) |
| SVG/path | SVG/ThorVG/Skia ([ADR-004](../../decisions/ADR-004-buy-vector-path-rasterization.md)) | path geometry/tessellation |
| Shader | GLSL/WGSL + manifest ([`shader-manifest-v1.md`](../specs/shader-manifest-v1.md)) | pipeline + reflection |
| Theme | JSON tokens | custom properties |
| Data | JSON/CSV/binary | app-owned buffers |

Assets are referenced by stable **asset ids**; file paths are build-time metadata, not runtime
lookups. The compiler/packager emits an asset manifest.

## 2. Loading pipeline

```text
asset id -> cache lookup -> async load/decode -> CPU resource
  -> (GPU resource upload) -> ready event -> UI thread applies
```

- Loading starts off the UI thread; the UI thread never blocks on file/network/decode.
- CPU decode has size/format limits (decompression bombs, huge dimensions); the values are in section 2.1.
- GPU upload happens on the render thread (see [`threading-and-memory.md`](threading-and-memory.md)) or via a staging queue; completion is queued.
- A placeholder is shown until ready; missing assets produce a deterministic error placeholder.
- Cancellation is supported when a node no longer needs the asset.

### 2.1 Image formats and limits

| Item | V1 rule |
|---|---|
| Accepted formats | PNG, JPEG and WebP, identified by file signature, not by extension. Other formats (GIF, BMP, TIFF, AVIF, SVG as raster) are rejected |
| Maximum file size | 16 MiB |
| Maximum width or height | 8192 pixels |
| Maximum decoded size | 64 MiB, computed as width x height x 4 bytes (RGBA8) |
| Color handling | Decoded to 8-bit RGBA, sRGB; embedded color profiles other than sRGB are ignored |
| Animation | Only the first frame of an animated WebP is used |

The compiler reads the file header (not the pixels) and checks format and limits for every `Image.src`
([`jsx-subset-v1.md`](../specs/jsx-subset-v1.md) section 11.1; codes `TESSERA_JSX_IMAGE_FORMAT` and
`TESSERA_JSX_IMAGE_SIZE`). The runtime decoder enforces the same limits before allocating, so a file
replaced after the build fails with the error placeholder of section 2 instead of exhausting memory. The
limits can be lowered or raised with `assets.imageMaxFileBytes`, `assets.imageMaxDimension` and
`assets.imageMaxDecodedBytes` ([`project-config-v1.md`](../specs/project-config-v1.md)). These values were
chosen by the assistant as starting points and are to be re-measured on target devices.

### 2.2 Decode at display size

The 64 MiB limit bounds a single decode; it does not make a list of thumbnails affordable (issue #14:
a 4000 x 3000 photo shown at 200 x 150 dp costs 48 MB decoded, but about 1 MB at 3x). A decode
therefore takes a `DecodeTarget` (`rendering/image/image_resize.hpp`):

- **Target:** `decode_target_for(display_w, display_h, dpr)` is `ceil(display x dpr)` per side rounded
  up to a bucket of the sequence 64, 96, 128, 192, 256, 384, ... (powers of two and midpoints, at
  most 50% above the request), so small layout changes keep hitting the same decode.
- **Size:** the image is reduced to the smallest size that covers the target box, keeps its aspect
  ratio and is never enlarged. A zero target means `original` (`Image decodeSize="original"`, for
  zoom and edit).
- **Filter:** a box filter in pure integer arithmetic, weighting color by alpha, so identical input
  gives byte-identical output on every platform.
- **Cache key:** `ImageDecodeCache` keys on content hash and target. A target that does not shrink
  the image shares the source-size entry; growing past a bucket decodes again. The cache budget
  counts the reduced raster. Limits are judged on the encoded image's size, so a reduced decode
  cannot slip past them.
- **Limit:** stb_image has no scaled decode, so PNG (and any JPEG the fast path declines) holds the
  full raster transiently during the call and frees it before it returns; the retained and uploaded
  memory is the reduced one.
- **Scaled JPEG:** libjpeg-turbo (plain C, no SIMD) decodes at 1/2, 1/4 or 1/8 inside the IDCT, the
  largest step that still covers the reduced size (`jpeg_scale_denominator`); the box filter
  finishes the last fraction. A 4000 x 3000 photo shown as a thumbnail never allocates its 48 MB
  raster, which lowers the peak, not only the retained size. CMYK, non-8-bit and corrupt data fall
  back to stb. A target that needs no reduction keeps using stb, so unscaled output is unchanged.
- **WebP:** libwebp decodes the format (stb has none). A still image is scaled inside the decoder
  (its own rescaler) straight into the result buffer, so the full raster never exists; an animated
  file decodes its first frame composed on the canvas and then goes through the box filter.
  libwebp keeps its SIMD paths, which are written to be bit-exact with its C code. Limits are
  judged on the encoded size before any pixel is allocated.
- **Wiring:** the Batched2D sink asks `image_decode_target(draw, device_scale)` for the target: the
  raster extent the draw will upload (explicit extents, else content box x scale), bucketed.
  `PaintImageStyle::decode_original` (`Image decodeSize="original"`) selects the source-size
  decode. Intrinsic size for fit and scale comes from `source_width/height`, so a reduced decode
  does not change layout or the texture key. The JSX prop `decodeSize="display" | "original"` (default `display`) is in the intrinsics schema and
  lowers to `props.decodeSize`; native codegen for `Image` maps it to `decode_original` once it exists.

## 3. GPU uploads

- Staging buffers are pooled and reused.
- Uploads are batched per frame; no per-texture command list/submit.
- Image layout transitions are owned by the backend/resource layer, not the UI.
- Samplers/filters are part of the texture desc; no hidden global state.
- Texture format/color space is explicit (`R8G8B8A8_UNORM` baseline; sRGB support is not scheduled and no decision has been made, see "B2 / known limits" in [`renderer.md`](renderer.md)).
- Mipmaps/compression are opt-in; document the color-space and filtering rules.

## 4. Caches and lifetime

| Cache | Key | Eviction |
|---|---|---|
| Texture | asset id + decode params | LRU (least recently used), not in current frame |
| Glyph atlas | face/size/glyph | LRU with frame-age guard |
| Pipeline | shader/vertex layout/blend | refcount + LRU |
| Path | SVG/path hash + quality | LRU |
| Theme | theme path/hash | replace on change |
| Custom element target | element id + pixel size/format | LRU, not in flight |

- Caches are bounded by bytes and item count. "Frame-age guard" means an entry used by a frame that is still in flight is not evicted.
- Resources used by in-flight frames are not destroyed; retire after completion.
- Hot reload (live replacement of an asset without restarting; see [`designer.md`](designer.md)) replaces an asset generation; old generation retires when unreferenced.
- Shutdown drains/retires caches deterministically.
- Custom elements share pipelines/meshes/textures through these caches; per-instance offscreen
  targets are bounded and retire after frames in flight. See
  [`custom-elements-v1.md`](../specs/custom-elements-v1.md).

## 5. Asset manifest

```json
{
  "manifestVersion": 1,
  "assets": [
    { "id": "img.logo", "type": "image", "path": "assets/logo.png", "hash": "sha256:..." },
    { "id": "font.inter", "type": "font", "path": "assets/Inter-Regular.ttf", "hash": "..." },
    { "id": "theme.default", "type": "theme", "path": "assets/theme.json", "hash": "..." }
  ]
}
```

- Paths are project-relative and validated (no absolute/traversal).
- Hashes drive incremental packaging and cache keys.
- The manifest is generated by the build; runtime uses ids.
- Licenses/notices for fonts/images are part of the package.
- **Font stack:** the bundle holds the faces and `fonts/stack.txt`, the ordered fallback list the
  host passes to `FallbackShaper` through `assets::load_font_stack` (text.md section 4).

## 6. Determinism

- Golden mode (deterministic test rendering, see [glossary](../reference/glossary.md)) uses committed assets only; no system fonts/network.
- Decode settings (color space, filtering, mipmaps) are fixed.
- Cache insertion order is deterministic because it determines atlas packing and therefore pixel output.
- Async completion order must not change pixels; results are applied at frame boundaries with
  deterministic ordering.

## 7. Diagnostics

- Load/decode/upload timings and bytes.
- Cache hit/miss/eviction; atlas pages and waste.
- Missing assets and fallback usage.
- GPU memory by category and peak.
- Hot-reload generation and retired resources.

## 8. Tests

- Loader tests for valid/invalid/oversized assets.
- Cache eviction and frame-age guard.
- Async cancellation and placeholder behavior.
- Upload batching and layout transitions (validation layers).
- Deterministic atlas packing and golden output.
- Hot reload: replace asset, old generation retires after in-flight frames.
- Path traversal/symlink rejection tests.

## 9. Open questions

- Build-size impact of the image decoder set (stb_image, libjpeg-turbo, libwebp).
- Texture compression (BC/ASTC/ETC2) and platform support.
- SVG rasterization backend (ThorVG vs lunasvg).
- Asset streaming for very large images/atlases.
- Hot reload of fonts and glyph atlas invalidation.
