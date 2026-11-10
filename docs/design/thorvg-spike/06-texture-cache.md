# Texture cache and upload path

> [<- Reproducing the spike](05-reproducing.md) | [SVG decode into path geometry ->](07-svg-decode.md)

## 10. Upload path: PathTextureCache

Follow-through on the spike: the seam produces an RGBA8 buffer, and `PathTextureCache` uploads it
into `rhi::Texture`s and keeps them in a bounded, deterministic LRU so the existing batched2d
textured-quad path can draw a rasterized path.

### API

- [`path_texture_key.hpp`](../../../include/tessera/rendering/path/path_texture_key.hpp) -
  `PathTextureKey` plus `make_path_texture_key(path_id, path_version, width, height, scale_milli,
  shape)`, which derives every pixel-affecting field from the shape so callers cannot forget one.
- [`path_texture_cache.hpp`](../../../include/tessera/rendering/path/path_texture_cache.hpp) -
  `PathTextureCacheConfig`, `CachedPathTexture { rhi::Texture* texture; rhi::Extent2D extent; }`,
  `PathTextureError`, `PathTextureCache`.
- `src/rendering/path/path_texture_cache.cpp` and `path_texture_key.cpp`; both build with
  `TESSERA_ENABLE_THORVG=OFF` because the cache takes a `PathRasterizer&` (the null backend works).

`acquire(key, shape)` returns the cached texture on a hit and rasterizes + uploads once on a miss.
The returned pointer is stable until `end_frame()` runs after that entry was evicted, or until
`invalidate_all()` / a device loss; re-acquire after those events. Eviction parks the texture
instead of destroying it, so draws already recorded this frame stay valid; the frame owner calls
`end_frame()` once the frame's commands are submitted (and older frames are no longer in flight).
The parking lot is bounded by the same `max_entries`/`max_bytes`; past that the oldest parked
texture is destroyed early and counted in `Stats::forced_releases`, which is the old behavior.
`ImageTextureCache` follows the same rule. Not thread-safe (one instance per rendering
thread, like the rest of the renderer).

### Key contract (exact)

All fields are integers; floats are quantized by `make_path_texture_key`, so equality is exact:

| Field | Meaning |
|---|---|
| `path_id`, `path_version` | caller identity + version of the geometry; the caller must bump the version when geometry changes (the cache cannot inspect geometry) |
| `width`, `height` | logical box in device pixels that the texture is drawn into |
| `scale_milli` | raster scale x1000 (1000 = 1x); the raster is `ceil(width * scale)` x `ceil(height * scale)` pixels and the path is scaled by the same factor, including stroke width |
| `fill_rule` | `FillRule` |
| `fill.present`, `fill.rgba` | fill presence and R, G, B, A packed by shift (layout-independent) |
| `stroke.present`, `stroke.rgba`, `stroke.width_milli`, `stroke.miter_milli`, `stroke.cap`, `stroke.join` | the full stroke signature |

### Capacity, eviction and memory bound

- Defaults: `max_entries = 64`, `max_bytes = 32 MiB`; bytes count RGBA8 pixel storage only
  (`4 * width * height * scale^2` per entry), so the memory bound is exact apart from one transient
  entry during creation.
- Deterministic LRU: the list front is the most recently used entry; a hit splices to the front, a
  miss inserts at the front, and eviction always takes the back until both bounds hold. A fixed
  request sequence therefore always evicts the same entries.
- A single raster larger than `max_bytes` is rejected with `entry_too_large` instead of flushing
  the cache on every request. Zero size/scale and scaled edges above 16384 px are
  `invalid_request`.
- A hit is a hash lookup plus a list splice: no rasterizer call, no device call, no allocation.

### Device loss (D7)

- The cache polls `Device::status()` on every `acquire()`. On `Lost` it destroys every texture
  through the RHI (unique_ptr owners, the only release path), returns `device_lost`, and records
  the loss. When the device reports `Ready` again the observed `generation()` increments and the
  next acquire re-rasterizes on demand.
- Callers that observe the loss themselves must call `notify_device_lost()` before
  `Device::recover()`: D7 recovery refuses (`ResourcesOutstanding`) while lost-generation resources
  are alive, and the cached textures are exactly such resources.
- Documented limitation (deferred): the RHI exposes device health but no generation counter, so a
  caller that recovers without ever notifying the cache and without an acquire observing `Lost`
  cannot be detected by the cache. The contract is notify-before-recover; a public
  `Device::generation()` would remove the caveat (RHI change, out of this slice).
- `invalidate_all()` drops entries for non-loss reasons (asset reload) and is explicit, never
  automatic.

### Drawing and color policy (D2)

`Batched2DRenderer::push_texture(dest_rect, *cached.texture, {0, 0, 1, 1}, tint)` draws the cached
raster as one textured quad with straight-alpha blending (the shader multiplies the vertex color by
the sampled texel; the pipeline uses `SRC_ALPHA`/`ONE_MINUS_SRC_ALPHA`).

- UNORM targets (deterministic goldens): `PathTextureCacheConfig::format = R8G8B8A8Unorm`, so the
  raster bytes are stored and sampled unchanged (byte-exact readback).
- sRGB targets: set `format = R8G8B8A8Srgb`. Sampling decodes the stored encoded value to linear and
  the sRGB attachment re-encodes on write, so the encoded bytes round-trip instead of being
  encoded twice. Blending still happens in the encoded space, as documented in D2.

### Tests

- `tests/unit/path_texture_cache_tests.cpp` (fake device, no GPU needed): hit/miss counts, stable
  handle, LRU eviction by count and bytes, entry-too-large/invalid requests, upload and rasterizer
  failure values, D7 generation invalidation, explicit `notify_device_lost()`, null-backend
  graceful behavior, cached bytes equal to a direct rasterizer run, ThorVG content equality when
  the option is ON, and a checked-in FNV of the raster bytes (cross-process determinism).
- `tests/gpu/batched2d/path_texture_draw_tests.cpp`: null-device draw plumbing always (one
  texture bind, one 6-vertex draw, submitted); with Vulkan it renders a cached 64x64 path texture
  into offscreen targets - UNORM bytes exact, straight-alpha blend against the GPU blend equation,
  sRGB texture into an sRGB target round-trip, 2x raster scale deterministic, two renders
  byte-identical.
- CTest names: `path_texture_cache`, `path_texture_draw` (the Vulkan readback cases are compiled
  only when the Vulkan backend is in the build).

### Measurements

Debug build, AMD Radeon RX 5300M, 512x512 raster, solid-fill rasterizer to isolate the upload path
(`path_texture_measure`, split out of `path_texture_draw_tests`); a ThorVG rasterization adds the task-12 numbers on top
(1.0-1.7 ms Release for the 512x512 icon/chart scene):

| Measurement | Value |
|---|---|
| First acquire (rasterize + texture create/upload, first use) | 8.9-13.2 ms |
| Steady-state re-upload (invalidate + acquire) | 1.86-1.97 ms average |
| Cache hit | 0.5-0.7 us average over 10,000 acquires |
| Texture bytes | 1,048,576 (1 MiB) |

### What remains

- Scene `Path` node and paint-pass integration (depends on the open paint-data decision).
- Asset integration of decoded SVG (see section 11): asset packaging, decoded-document cache, scene
  `Path`/`Image` nodes.
- Damage-aware re-rasterization / partial texture updates, and a path atlas or tiling strategy for
  many small icons (the current cache is one texture per entry).
- Public `Device::generation()` so loss invalidation does not rely on the notify-before-recover
  contract.
