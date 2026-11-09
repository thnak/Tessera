# Decoded-document cache and document textures

> [<- SVG decode into path geometry](07-svg-decode.md)

## 12. Decoded-document cache and document-to-texture path

So an SVG asset is parsed once and then rasterized for every size/scale without re-decoding, while
memory stays bounded and the texture path reuses everything section 10 already built.

### API

- [`svg_document_cache.hpp`](../../../include/tessera/rendering/path/svg_document_cache.hpp):
  `SvgDocumentKey { content_hash, byte_size, SvgLimits }` built by `make_svg_document_key()`;
  `SvgDocumentCacheConfig { max_entries = 64, max_shapes = 4096, max_segments = 131072 }`;
  `SvgDocumentCache::acquire(key, bytes)` / `acquire(bytes)` returning
  `std::expected<const SvgDocument*, SvgError>`, plus `invalidate(key)`, `invalidate_all()` and
  `Stats { hits, misses, decodes, evictions, invalidations, entries, shapes, segments }`.
- [`svg_texture.hpp`](../../../include/tessera/rendering/path/svg_texture.hpp): `SvgTextureKey
  { document_id, document_version, width, height, scale_milli }` built by
  `make_svg_texture_key(id, version, document, scale_milli = 1000)`, and
  `acquire_svg_texture(PathTextureCache&, const SvgTextureKey&, const SvgDocument&)` returning
  `std::expected<CachedPathTexture, PathTextureError>`.

### Key, bounds and eviction

- **Content key:** FNV-1a 64 over the raw SVG bytes plus the byte size and the full `SvgLimits`
  signature, so equal bytes under different decode policies never alias. The hash is stable across
  runs, processes and working directories (checked in the test). It is a cache key, not a security
  boundary: a deliberate 64-bit collision could alias two assets, which the byte size narrows
  further (documented tradeoff; a content-hash upgrade can be swapped in without changing callers).
- **Bounds:** defaults 64 entries, 4096 decoded shapes and 131072 segments in total. Both budgets
  are enforced exactly; a single document above a budget is rejected with
  `SvgError::limit_exceeded` and never cached.
- **Eviction:** LRU with the front as most recent; a hit is a hash lookup plus a list splice (no
  decode, no allocation), a miss decodes and inserts at the front, and eviction takes the back
  until both budgets hold. The order is total, so a fixed request sequence evicts the same entries.
  The returned `const SvgDocument*` is stable until that entry is evicted or invalidated.
- **Failures are values and are never cached:** malformed/unsupported/too-large/backend-unavailable
  return the `SvgError` from `load_svg()` (or `limit_exceeded` for a document over the total
  budget); the next acquire retries and increments `decodes`. There is no partial entry.
- With ThorVG OFF the cache still links and returns `SvgError::backend_unavailable`; nothing is
  stored.

### Document → texture rule

`acquire_svg_texture()` rasterizes **every shape of the document in one pass** and uploads it as
one texture per (document identity, logical size, raster scale), through the existing
`PathTextureCache` (so the LRU/byte bound, UNORM-vs-sRGB format rule from D2 and the D7
device-loss generation handling are inherited unchanged). The texture key sets
`PathTextureKey::document = true`, so a whole-document composite can never collide with a
single-shape key of the same id/version/size/scale.

Batching rule: a static SVG document is one draw call, overlapping shapes and group opacity
composite exactly as the document specifies, and one decoded document feeds every size/scale
variant. **Per-shape textures** (`PathTextureCache::acquire(key, PathShape)`) still win when shapes
move or restyle independently, when per-shape damage/hit regions matter, or when one shape is
shared by several documents; for a single-path icon the document wrapper adds nothing over the
per-shape call.

### Evidence and tests

- **Decode once:** the document-cache test asserts `stats().decodes == 1` after the first decode
  while the same document is used for the 1x and 2x rasterizations and the cache-hit acquire; the
  end-to-end GPU test asserts the same before it rasterizes.
- **Texture equality:** the produced document texture bytes equal a direct
  `PathRasterizer::rasterize(document.shapes)` for the same key (test), and are deterministic:
  checked-in cross-process FNV-1a values `0xa0172eea2bc1df50` (1x) and `0xa06d760467d58f2f` (2x) for
  `icon_shapes.svg`.
- `svg_document_cache` unit test (registered from `src/rendering/path/CMakeLists.txt`): key
  stability (checked-in `0x14bc863136cb38d9`), decode-once-then-hit, LRU eviction by entry count
  and by shape and segment budgets, over-budget rejection, `invalidate()`/`invalidate_all()`,
  malformed/unsupported/empty inputs never cached, ThorVG-OFF `backend_unavailable`, document
  texture equality, namespace disjointness from single-shape keys.
- `path_texture_draw`: SVG end-to-end now goes document cache → one document texture → batched2d
  textured quad; the null-device assertions run in every configuration, and with Vulkan
  (Radeon RX 5300M) the readback checks the opaque rect pixel, the clear pixel outside the shapes
  and the translucent circle pixel, plus byte-identical repeat renders.

### Measurements

`icon_shapes.svg` (64x64, 2 shapes, 11 segments; texture 16 KiB at 1x, 64 KiB at 2x). Three runs
per row; `thorvg_spike --svg ... --svg-measure` for the document cache, the Vulkan measure mode of
`path_texture_draw_tests` for the document texture:

| Build | Document first acquire (decode) | Cached acquire | Document memory | Texture memory |
|---|---|---|---|---|
| Debug | 0.43-1.57 ms | 0.7-0.9 us | 480 B | 16 KiB |
| Release | 0.13-0.21 ms | <=0.1 us (timer floor) | 464 B | 16 KiB |

| Debug GPU path (RX 5300M) | Value |
|---|---|
| Document texture first acquire (rasterize + Vulkan upload) | 0.56-0.82 ms |
| Cached document-texture acquire | 0.9-1.0 us |
| Decode in the same run | 0.23-0.25 ms |

Caching removes the decode from every rasterization: a cached acquire is ~0.5% of a Debug decode
and below timer resolution in Release, at 480 B of retained geometry versus 16 KiB (1x) or 64 KiB
(2x) of texture per document.

### What remains (document cache side)

- Asset packaging and loading policy, and hot-reload/invalidation by file mtime or content version
  (the cache exposes `invalidate()`/`invalidate_all()` for the owner to drive).
- Per-document partial rasterization/tiling and a document texture atlas for many small icons (the
  current rule is one texture per document/size/scale inside the shared texture-cache bounds).
- Scene `Path`/`Image` nodes that own document lifetimes and feed `acquire_svg_texture()`.
- A content hash with a larger digest if the cache ever spans untrusted assets; the key struct is
  ready for it.
