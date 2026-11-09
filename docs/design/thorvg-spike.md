# ThorVG integration spike (rendering P1, decision D1)

Status: **complete, recommendation Go**. The project owner approved D1 (ThorVG first) on
2026-11-09 in [`rendering-production-plan.md`](rendering-production-plan.md) section 3; this note
records the P1 spike that plan asks for: dependency bring-up, the `PathRasterizer` seam, build and
runtime measurements, determinism evidence, and what is deliberately left out before the full path
integration. Related:
[`dependencies.md`](../specs/dependencies.md#1-gpu-and-rendering),
[`THIRD_PARTY_NOTICES.md`](../../THIRD_PARTY_NOTICES.md),
[ADR-004](../../decisions/ADR-004-buy-vector-path-rasterization.md),
[ADR-012](../../decisions/ADR-012-static-linking.md),
[ADR-013](../../decisions/ADR-013-project-license-mit.md).

## 1. Question and verdict

The spike had to answer four questions before ThorVG is pinned as a production dependency
([ADR-004](../../decisions/ADR-004-buy-vector-path-rasterization.md)):

1. Can ThorVG be built reproducibly here, statically, with bounded size and build time?
2. Can it rasterize Tessera's path model deterministically into an RGBA8 buffer, so the output
   can be a golden and can be uploaded through the RHI?
3. Does ThorVG provide a Vulkan path, or is CPU rasterization the only viable V1 route?
4. Does the seam keep ThorVG out of public headers and out of UI code?

**Verdict: Go.** A meson-only upstream, a local CMake wrapper and a CPU rasterizer are enough for
V1. Nothing in the result depends on a Vulkan backend inside ThorVG, and the build/runtime cost is
small: ~12 s clean build of the ThorVG-dependent target, 894 KiB static archive (Release), 336 KiB
added to the spike executable, ~1 ms first render of a 512x512 icon/chart scene, 6.5 MiB peak
working set, and byte-identical output across runs and across Debug/Release. Skia Graphite is not
needed and stays the documented fallback only.

## 2. Dependency bring-up

### Pin

| Item | Value |
|---|---|
| Version | ThorVG **v1.1.3** (tag commit `589bc4120f2097fb1b135e552c2ef39f6581612b`) |
| Source | `https://github.com/thorvg/thorvg/archive/refs/tags/v1.1.3.tar.gz` |
| SHA-256 | `ba9957aeeadb4151d4486f11c638ae118c00210fa4906daa7b30143aa341e690` |
| License | MIT, copyright 2020 - 2026 ThorVG Project |
| Vendoring | **no**: fetched at configure time by `FetchContent`, verified by `URL_HASH` |

The pin lives in [`cmake/ThorVG.cmake`](../../cmake/ThorVG.cmake); the two values must be updated
together. The archive was verified against a shallow clone of the tag: all selected sources and
`inc/` headers are byte-identical.

### Build wrapper (why not `FetchContent_MakeAvailable` alone)

ThorVG v1.1.3 is **meson-only**: the archive contains no `CMakeLists.txt` anywhere (verified), so
`FetchContent_MakeAvailable()` would only download and extract it. The wrapper in
`cmake/ThorVG.cmake` therefore compiles the pinned source set directly:

- sources: `src/common/**`, `src/renderer/**` minus `src/renderer/gpu_engine/**` (GL/WebGPU
  backends), `src/loaders/svg/**`, `src/loaders/raw/**` - 37 translation units;
- include paths: `inc/`, `src/common/`, `src/renderer/`, `src/renderer/cpu_engine/`,
  `src/loaders/raw/`, `src/loaders/svg/`, plus a generated directory holding `config.h`;
- `config.h` mirrors the meson feature options, with only the features the seam uses enabled:
  `THORVG_VERSION_STRING "1.1.3"`, `THORVG_CPU_ENGINE_SUPPORT`,
  `THORVG_SVG_LOADER_SUPPORT`, `THORVG_PARTIAL_RENDER_SUPPORT`, `THORVG_FILE_IO_SUPPORT`; every
  other `THORVG_*` feature stays undefined;
- `TVG_STATIC` (PUBLIC, selects the non-DLL API path), `NOMINMAX` on Windows, C++17 for the
  third-party sources, and `-w`/`/W0` so Tessera's `-Werror` policy does not apply to them.

An `ExternalProject` + meson build was rejected: it adds a build-time tool dependency (meson,
Python) without changing the produced artifact. A fork or vendored copy was rejected as well.
Nothing from ThorVG is vendored; the source tree lives in the CMake build directory only.

### Option

`TESSERA_ENABLE_THORVG` (root `CMakeLists.txt`) is **OFF** by default. When OFF:

- no download, no `tessera_thorvg` target, no ThorVG compile or link command;
- `tessera_path` still builds and provides the seam with its null backend, so non-ThorVG builds
  link;
- existing targets and binaries are unchanged. This was proven byte-for-byte: a Release build of
  three pre-existing targets (`merger_poc`, `tessera_schema_dump`, `image_diff_tests`) from the
  working tree with the option present and OFF produced identical SHA-256 hashes to the same
  targets built from a copy of the tree with the option/wrapper additions removed (link
  timestamps pinned with `-Wl,/Brepro` for the comparison; without that flag only the PE header
  timestamp differs).

When ON, `third_party/CMakeLists.txt` includes the wrapper and builds `tessera_thorvg`;
`nix`/CI users get the same target, no system ThorVG is used.

## 3. The PathRasterizer seam

Public API (no third-party include in any public header):

- [`path_model.hpp`](../../include/tessera/rendering/path/path_model.hpp): `PointF`, `Rgba8`,
  `FillRule` (non-zero/even-odd), `LineCap` (butt/round/square), `LineJoin`
  (miter/round/bevel), `PathSegment` (`move_to`/`line_to`/`quad_to`/`cubic_to`/`close`), `Path`,
  `StrokeStyle` (color, width, cap, join, miter limit), `PaintStyle` (optional fill + optional
  stroke), `PathShape`, and `RasterTarget` (RGBA8, straight alpha, explicit `width`, `height`,
  `stride_bytes`; caller-owned pixels).
- [`path_rasterizer.hpp`](../../include/tessera/rendering/path/path_rasterizer.hpp):
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

Determinism is a hard test requirement ([ADR-009](../../decisions/ADR-009-deterministic-headless-golden-tests.md)),
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

## 5. Measurements

Method: Windows 11, clang 22.1.5 (LLVM, GNU-like driver), CMake 4.1.1, Ninja, `-j12`, Vulkan
backend OFF (`TESSERA_BUILD_VULKAN=OFF`), `TESSERA_WARNINGS_AS_ERRORS=ON`. Timed builds run with
the compiler cache disabled so the numbers are cold. Configure timings include the FetchContent
downloads (network), which is why they are much larger than the compile time.

| Metric | Release | Debug | Notes |
|---|---|---|---|
| Clean configure, option ON | 72.0 s | 49.6 s | includes fetching ThorVG (3.6 MiB), FreeType and HarfBuzz |
| Clean build of `thorvg_spike` | 12.2 s | 12.6 s | 50 Ninja steps, 37 ThorVG translation units, `-j12` |
| `tessera_thorvg` static archive | 915,564 B (894 KiB) | 4,099,462 B (3.91 MiB) | Debug size is debug info |
| `thorvg_spike.exe` | 569,344 B (556 KiB) | 1,381,888 B (1.32 MiB) | links seam + golden + stb |
| Null-only reference exe (same sources, no ThorVG) | 225,792 B | - | delta attributable to ThorVG: **343,552 B (335.5 KiB)** |
| Startup (process entry to rasterizer ready) | 0.02-0.04 ms | 0.06-0.49 ms | `--backend thorvg` |
| First render, 512x512, 15 shapes | 1.0-1.7 ms | 4.0-4.8 ms | `rasterize()` incl. `draw()`/`sync()` |
| Process total incl. PNG write | 13-26 ms | 111-122 ms | Debug PNG encoding dominates at `-O0` |
| Peak working set, 512x512 | 6.53 MiB | - | includes pixel buffer, ThorVG, and PNG encode |

Interpretation:

- **Build cost is acceptable.** 12 s on top of a clean build, ~894 KiB Release archive; the final
  binary grows by ~336 KiB because the linker drops unused ThorVG code. That is far below the
  size risk the plan flagged for ICU/Skia.
- **Startup is negligible**; first-render cost is CPU rasterization of the path set, not
  initialization. A single static 512x512 icon frame is ~1 ms in Release, so a UI with a handful
  of icon-sized paths is not CPU-bound at 60 Hz. Large animated path sets will need caching and
  damage-scissored re-rasterization (follow-up, not a spike requirement).
- **Memory is small**: no per-frame allocation is visible beyond ThorVG's raster pool; the peak
  includes the 1 MiB RGBA buffer plus PNG encoding scratch.

### Vulkan viability

**There is no ThorVG Vulkan engine.** v1.1.3 exposes `SwCanvas` (CPU), `GlCanvas` (OpenGL/EGL) and
`WgCanvas` (WebGPU); the compiled `gpu_engine/` directory holds only the GL and WebGPU backends.
RHI integration therefore is, and must be, **CPU rasterize into an RGBA8 buffer, then upload the
buffer through the Tessera RHI** (a texture upload, not a new renderer backend). On the design-time
WebGPU side, ThorVG's `WgCanvas` is not a substitute for Dawn and is out of scope here; the seam
keeps that option open without committing to it.

## 6. Layering and warning policy

- Only `src/rendering/path/thorvg_path_rasterizer.cpp` includes a ThorVG header. The
  configure-time gate in `src/rendering/path/CMakeLists.txt` scans every public header, `src/` and
  `tools/` source for `#include <thorvg...>`/`"thorvg..."` and fails configuration if any file
  other than that adapter matches; it runs in **every** configuration, including the default
  build where ThorVG is OFF.
- Public headers expose only seam types; UI code can only reach the rasterizer through
  `PathRasterizer`.
- Third-party sources compile with `-w`/`/W0` + C++17; Tessera sources (seam, spike tool) stay
  under `-Werror` and the project's C++23 baseline. The spike builds with
  `TESSERA_WARNINGS_AS_ERRORS=ON`.

## 7. Deliberately not done

- **No renderer backend integration.** No RHI texture upload, no `Path` scene node, no paint-pass
  wiring, no SVG asset loading path (the SVG loader compiles but is not exercised by the seam).
- **No Vulkan/WebGPU backend work** inside ThorVG, and no attempt to make `GlCanvas`/`WgCanvas`
  usable; the spike never creates a GPU surface.
- **No seam features beyond V1**: no gradients, dashes, masks, text-as-path, blend modes, or
  clipping. They are legal ThorVG features but not part of the seam yet.
- **No threading tuning**: worker threads are pinned to zero for determinism. A future
  deterministic tiling policy is a separate decision.
- **No performance characterization at scale**: the spike measures one 512x512 frame and a fixed
  icon/chart set, not animation, damage redraw, or large SVG scenes. Caching/tiling design comes
  with the real integration.

## 8. Recommendation: Go

Proceed with the full layout of the plan's P1 item 1: keep `TESSERA_ENABLE_THORVG` (default OFF
until the path feature ships), add the `Path` scene node and paint-pass integration, upload the
RGBA8 buffer through the RHI, add the SVG asset path, then revisit caching/tiling and MSAA (D5)
with real frames. No re-pin or alternative backend is needed for that work; the seam can stay
unchanged, which is the point of it.

The remaining decision for the Lead/owner is not Go/No-Go but **when** the option flips to ON by
default; this spike keeps it OFF so the default build and its artifacts stay unchanged.

## 9. Reproducing

```powershell
cmake -S . -B build/tvg -G Ninja -DCMAKE_BUILD_TYPE=Release `
  -DCMAKE_CXX_COMPILER=clang++ -DTESSERA_BUILD_VULKAN=OFF `
  -DTESSERA_ENABLE_THORVG=ON
cmake --build build/tvg --target thorvg_spike
.\build\tvg\tools\poc\thorvg_spike\thorvg_spike.exe --out icons.png
.\build\tvg\tools\poc\thorvg_spike\thorvg_spike.exe --compare tests\golden\thorvg\icons.png
ctest --test-dir build/tvg -R thorvg_spike_golden --output-on-failure
```

`--backend null` exercises the seam without painting (useful to compare binary sizes and to check
plumbing). `--raw path` writes the raw RGBA8 buffer, and `--tolerance`/`--max-mismatch` control the
golden comparison for future platform portability checks.

## 10. Upload path: PathTextureCache

Follow-through on the spike: the seam produces an RGBA8 buffer, and `PathTextureCache` uploads it
into `rhi::Texture`s and keeps them in a bounded, deterministic LRU so the existing batched2d
textured-quad path can draw a rasterized path.

### API

- [`path_texture_key.hpp`](../../include/tessera/rendering/path/path_texture_key.hpp) -
  `PathTextureKey` plus `make_path_texture_key(path_id, path_version, width, height, scale_milli,
  shape)`, which derives every pixel-affecting field from the shape so callers cannot forget one.
- [`path_texture_cache.hpp`](../../include/tessera/rendering/path/path_texture_cache.hpp) -
  `PathTextureCacheConfig`, `CachedPathTexture { rhi::Texture* texture; rhi::Extent2D extent; }`,
  `PathTextureError`, `PathTextureCache`.
- `src/rendering/path/path_texture_cache.cpp` and `path_texture_key.cpp`; both build with
  `TESSERA_ENABLE_THORVG=OFF` because the cache takes a `PathRasterizer&` (the null backend works).

`acquire(key, shape)` returns the cached texture on a hit and rasterizes + uploads once on a miss.
The returned pointer is stable until that entry is evicted, `invalidate_all()` runs, or a device
loss is observed; re-acquire after those events. Not thread-safe (one instance per rendering
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
(`path_texture_draw_tests --measure`); a ThorVG rasterization adds the task-12 numbers on top
(1.0-1.7 ms Release for the 512x512 icon/chart scene):

| Measurement | Value |
|---|---|
| First acquire (rasterize + texture create/upload, first use) | 8.9-13.2 ms |
| Steady-state re-upload (invalidate + acquire) | 1.86-1.97 ms average |
| Cache hit | 0.5-0.7 us average over 10,000 acquires |
| Texture bytes | 1,048,576 (1 MiB) |

### What remains

- Scene `Path` node and paint-pass integration (depends on the open paint-data decision).
- SVG asset loader into the same seam.
- Damage-aware re-rasterization / partial texture updates, and a path atlas or tiling strategy for
  many small icons (the current cache is one texture per entry).
- Public `Device::generation()` so loss invalidation does not rely on the notify-before-recover
  contract.

