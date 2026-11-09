# SVG decode into path geometry

> [<- Texture cache and upload path](06-texture-cache.md) | [Decoded-document cache and document textures ->](08-document-cache.md)

## 11. SVG assets: static-icon decode into path geometry

The last load-side seam: `load_svg()` decodes an SVG byte span into the same `PathShape` model that
`PathRasterizer` and `PathTextureCache` already consume, so an icon asset goes
**decode -> rasterize -> cache -> textured quad** with no new renderer path. Only
`src/rendering/path/thorvg_svg_loader.cpp` includes `<thorvg.h>`; the configure-time layering gate
now allowlists exactly the two adapters (`thorvg_path_rasterizer.cpp`, `thorvg_svg_loader.cpp`).

### API

[`svg_loader.hpp`](../../../include/tessera/rendering/path/svg_loader.hpp):

```cpp
struct SvgLimits {
    std::size_t max_input_bytes = 1024U * 1024U; // 1 MiB
    std::uint32_t max_shapes = 1024;             // Shape paints in the decoded tree
    std::uint32_t max_depth = 32;                // nested group/scene depth
    std::uint32_t max_segments = 65536;          // total path segments
    std::uint32_t max_extent = 16384;            // document width/height bound
};

struct SvgDocument {
    std::vector<PathShape> shapes; // absolute document coordinates, painter order
    std::uint32_t width = 0;       // resolved document size (viewBox mapped to width/height)
    std::uint32_t height = 0;
};

enum class SvgError { invalid_argument, input_too_large, malformed, unsupported, limit_exceeded,
                      backend_unavailable, backend_failure };

std::expected<SvgDocument, SvgError> load_svg(std::span<const std::uint8_t>, SvgLimits = {});
std::expected<SvgDocument, SvgError> load_svg(std::span<const char>, SvgLimits = {});
bool svg_backend_available() noexcept; // false without TESSERA_ENABLE_THORVG
```

- Groups and `transform` attributes are **baked** into the path points (the seam has no transform
  stack); `Paint::opacity` and `fill-opacity`/`stroke-opacity` are folded into the per-shape color
  alpha. Document coordinates stay in the document's unit space, so the caller draws the document
  at `width x height` logical pixels and `PathTextureCache(scale_milli)` supplies 2x rasterization.
- Never throws across the seam: input/limits are validated up front and the whole ThorVG
  interaction is wrapped, returning `backend_failure` for internal problems.
- With ThorVG OFF the API still links and returns `SvgError::backend_unavailable` (a value), never
  a link error; `svg_backend_available()` lets callers pick a fallback.

### Supported subset

Decoded by ThorVG and representable in the path model:

| Feature | Notes |
|---|---|
| `<svg>` root, `viewBox`, `width`/`height` | document size = resolved picture size, rounded up; viewBox scale is baked into geometry |
| `<rect>`, `<circle>`, `<ellipse>`, `<line>`, `<polyline>`, `<polygon>`, `<path>` | arcs/ellipses arrive as cubics; `M/L/C/Z` map to `move_to`/`line_to`/`cubic_to`/`close` |
| `<g>` groups and `transform` (translate/scale/rotate/matrix) | composed and baked; `sqrt(|det|)` scales stroke width under non-uniform transforms (documented approximation) |
| solid `fill`/`stroke` colors, `fill-opacity`, `stroke-opacity`, `opacity` | folded into the color alpha; group opacity is applied per shape (see limits) |
| `stroke-width`, `stroke-linecap`, `stroke-linejoin`, `stroke-miterlimit` | cap/join orders match the seam enums exactly |
| `fill-rule: nonzero/evenodd` | maps to `FillRule` |
| `display:none` / `visibility:hidden` | subtree ignored |
| `title`, `desc`, `metadata`, comments, CDATA | ignored |

Not representable and **rejected with `SvgError::unsupported`** (loud, never a silently incomplete
icon): `<text>`/`<tspan>`/`<textPath>`, `<image>` and `foreignObject`, `<filter>` and any `fe*`
primitive, `linearGradient`/`radialGradient`/`conicGradient`/`pattern`, `clipPath`/`mask`,
`animate`/`animateTransform`/`animateMotion`/`set`, `use`/`switch`/`marker`/`script`, any
`href`/`xlink:href` attribute, any `url(...)` IRI reference (paint servers, clip paths, filters),
and `DOCTYPE`/external entities. As a post-load safety net the decoder also rejects gradient fills,
`Paint::clip()`, masks, and any `Text`/`Picture` node that reaches the walker.

**Ignore vs reject rule:** structures that cannot change what is drawn (metadata, comments,
presentation attributes ThorVG does not implement, invisible subtrees) are ignored; anything that
*would* draw something we cannot represent is rejected. Malformed XML (loader `Result != Success`)
is `SvgError::malformed`; a well-formed document with no drawable shapes is a valid empty document.

### Bounds and determinism

- Input size, shape count, tree depth, total segments and document extent are bounded by
  `SvgLimits`; exceeding them returns `input_too_large`/`limit_exceeded` instead of exhausting
  memory or recursion. The pre-scan is linear in the input and rejects unsupported tags before the
  loader runs.
- Geometry is a pure function of the bytes and the pinned ThorVG version: two loads of the same
  bytes produce identical floats (no re-rounding), and rasterization at 1x and 2x is
  byte-deterministic. Checked evidence:
  - `tests/golden/thorvg/svg_icon.png` (64x64, from
    `tests/fixtures/svg/icon_shapes.svg`), compared exactly by the `thorvg_svg_golden` CTest;
    the PNG is identical across runs and across Debug/Release.
  - `svg_loader` unit test asserts FNV-1a of `stroke_path.svg` rasterized at 1x
    (`0xaf7e9adbb9f450e3`) and of the 2x `PathTextureCache` upload (`0x4363862c336bf9c3`), so a
    second process and CI verify the same bytes.
- Deliberate golden update path: regenerate with
  `thorvg_spike --svg tests/fixtures/svg/icon_shapes.svg --out tests/golden/thorvg/svg_icon.png`,
  inspect the image, and update the checked-in PNG together with any fixture change (same rule as
  the other path goldens).

### Fixtures

`tests/fixtures/svg/` (hand-authored for this repository, MIT-compatible, no third-party assets):
`icon_shapes.svg` (rect + translucent circle -> golden), `stroke_path.svg` (single filled path,
used for the cache/GPU end-to-end), `group_transform.svg` (translate+scale group), `viewbox_scale.svg`
(viewBox 40x20 into an 80x40 document), `unsupported_text.svg` (rejected), `malformed.svg`
(truncated, rejected).

### Tests

- `svg_loader` (unit, registered from `src/rendering/path/CMakeLists.txt`): subset geometry with
  exact baked coordinates, fill/stroke/opacity folding, viewBox scaling, reject-vs-ignore cases,
  bounds (input/shape/depth/segment), two-load determinism, 1x/2x raster hashes; in the OFF build it
  asserts `backend_unavailable` instead of skipping.
- `thorvg_svg_golden`: `thorvg_spike --svg icon_shapes.svg --compare golden`.
- `path_texture_draw`: SVG end-to-end - decode, cache upload, and a batched2d textured quad; the
  null-device draw assertions run in every configuration, and with Vulkan (Radeon RX 5300M) the
  readback checks the SVG fill/alpha pixel and the clear pixel outside the shape, plus
  byte-identical repeat renders.

### Measurements

Debug build, 64x64 (80x40 for the viewBox fixture), AMD Radeon RX 5300M / clang 22.1.5, three runs
per fixture (`thorvg_spike --svg ...` prints `svg_load_ms` and `first_render_ms`):

| Fixture | Decode (ms) | Rasterize (ms) |
|---|---|---|
| `icon_shapes.svg` | 0.65-0.78 | 0.16-0.22 |
| `stroke_path.svg` | 0.63-0.78 | 0.17-0.19 |
| `group_transform.svg` | 0.65-0.94 | 0.12-0.19 |
| `viewbox_scale.svg` | 0.54-0.72 | 0.10-0.14 |

The 64x64 icon at 2x rasterizes a 128x128 texture in ~0.28 ms. These are Debug numbers; asset
decode is a load-time cost, so a production asset cache would key on the same document identity.

### What remains (SVG side)

- Text, filters, gradients, animation and external resources (rejected today; each needs a seam
  extension, not just a loader flag).
- Clip paths/masks (need a clip stack in the rasterizer seam).
- Asset packaging/loading policy (fonts, external images, DTD) and the scene `Path`/`Image` nodes
  that consume `SvgDocument`; decoding itself is cached by section 12.
